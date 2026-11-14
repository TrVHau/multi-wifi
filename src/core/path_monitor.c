#include "multiwifi/core/path_monitor.h"
#include "multiwifi/core/logging.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#define DEFAULT_HEALTH_CHECK_INTERVAL_MS 2000
#define DEFAULT_FAILURE_THRESHOLD 3
#define DEFAULT_RECOVERY_THRESHOLD 3
#define DEFAULT_COOLDOWN_MS 5000
#define ROLLING_WINDOW_SIZE 10

enum {
    SIGNAL_PATHS_CHANGED,
    LAST_SIGNAL
};

static guint s_signals[LAST_SIGNAL] = { 0 };

typedef struct {
    MultiWifiPathInfo *info;
    guint8 probe_history[ROLLING_WINDOW_SIZE]; /* 1 for success, 0 for failure */
    guint probe_history_count;
    guint probe_history_idx;
} PathEntry;

struct _MultiWifiPathMonitor {
    GObject parent_instance;
    GPtrArray *paths; /* PathEntry* */
    GMutex mutex;
    guint timer_id;

    guint interval_ms;
    guint failure_threshold;
    guint recovery_threshold;
    guint cooldown_ms;

    MultiWifiProbeFunc custom_probe_fn;
    gpointer custom_probe_data;
};

G_DEFINE_TYPE(MultiWifiPathMonitor, multiwifi_path_monitor, G_TYPE_OBJECT)

MultiWifiPathInfo *multiwifi_path_info_new(const gchar *ifname)
{
    MultiWifiPathInfo *info = g_new0(MultiWifiPathInfo, 1);
    info->ifname = g_strdup(ifname);
    info->state = MULTIWIFI_PATH_STATE_UNKNOWN;
    info->status_reason = g_strdup("Initialized");
    info->rtt_ms = 0.0;
    info->min_rtt_ms = 0.0;
    info->max_rtt_ms = 0.0;
    info->jitter_ms = 0.0;
    info->packet_loss_percent = 0;
    info->consecutive_successes = 0;
    info->consecutive_failures = 0;
    info->last_probe_time_us = 0;
    info->last_state_change_us = g_get_monotonic_time();
    info->is_eligible = FALSE;
    return info;
}

MultiWifiPathInfo *multiwifi_path_info_copy(const MultiWifiPathInfo *src)
{
    if (src == NULL) {
        return NULL;
    }

    MultiWifiPathInfo *dst = g_new0(MultiWifiPathInfo, 1);
    dst->ifname = g_strdup(src->ifname);
    dst->gateway = g_strdup(src->gateway);
    dst->source_ip = g_strdup(src->source_ip);
    dst->state = src->state;
    dst->status_reason = g_strdup(src->status_reason);
    dst->rtt_ms = src->rtt_ms;
    dst->min_rtt_ms = src->min_rtt_ms;
    dst->max_rtt_ms = src->max_rtt_ms;
    dst->jitter_ms = src->jitter_ms;
    dst->packet_loss_percent = src->packet_loss_percent;
    dst->consecutive_successes = src->consecutive_successes;
    dst->consecutive_failures = src->consecutive_failures;
    dst->last_probe_time_us = src->last_probe_time_us;
    dst->last_state_change_us = src->last_state_change_us;
    dst->is_eligible = src->is_eligible;

    return dst;
}

void multiwifi_path_info_free(MultiWifiPathInfo *info)
{
    if (info == NULL) {
        return;
    }

    g_free(info->ifname);
    g_free(info->gateway);
    g_free(info->source_ip);
    g_free(info->status_reason);
    g_free(info);
}

const gchar *multiwifi_path_state_to_string(MultiWifiPathState state)
{
    switch (state) {
    case MULTIWIFI_PATH_STATE_ONLINE:
        return "Online";
    case MULTIWIFI_PATH_STATE_DEGRADED:
        return "Degraded";
    case MULTIWIFI_PATH_STATE_LOCAL_ONLY:
        return "Local Only";
    case MULTIWIFI_PATH_STATE_PROBING:
        return "Probing";
    case MULTIWIFI_PATH_STATE_OFFLINE:
        return "Offline";
    case MULTIWIFI_PATH_STATE_UNKNOWN:
    default:
        return "Unknown";
    }
}

static PathEntry *path_entry_new(const gchar *ifname)
{
    PathEntry *entry = g_new0(PathEntry, 1);
    entry->info = multiwifi_path_info_new(ifname);
    return entry;
}

static void path_entry_free(PathEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    multiwifi_path_info_free(entry->info);
    g_free(entry);
}

static void multiwifi_path_monitor_dispose(GObject *object)
{
    MultiWifiPathMonitor *self = MULTIWIFI_PATH_MONITOR(object);

    if (self->timer_id > 0) {
        g_source_remove(self->timer_id);
        self->timer_id = 0;
    }

    g_mutex_lock(&self->mutex);
    if (self->paths != NULL) {
        g_ptr_array_unref(self->paths);
        self->paths = NULL;
    }
    g_mutex_unlock(&self->mutex);

    G_OBJECT_CLASS(multiwifi_path_monitor_parent_class)->dispose(object);
}

static void multiwifi_path_monitor_finalize(GObject *object)
{
    MultiWifiPathMonitor *self = MULTIWIFI_PATH_MONITOR(object);
    g_mutex_clear(&self->mutex);
    G_OBJECT_CLASS(multiwifi_path_monitor_parent_class)->finalize(object);
}

static void multiwifi_path_monitor_class_init(MultiWifiPathMonitorClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_path_monitor_dispose;
    oclass->finalize = multiwifi_path_monitor_finalize;

    s_signals[SIGNAL_PATHS_CHANGED] = g_signal_new(
        "paths-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL,
        NULL,
        NULL,
        G_TYPE_NONE,
        0
    );
}

static void multiwifi_path_monitor_init(MultiWifiPathMonitor *self)
{
    g_mutex_init(&self->mutex);
    self->paths = g_ptr_array_new_with_free_func((GDestroyNotify)path_entry_free);
    self->interval_ms = DEFAULT_HEALTH_CHECK_INTERVAL_MS;
    self->failure_threshold = DEFAULT_FAILURE_THRESHOLD;
    self->recovery_threshold = DEFAULT_RECOVERY_THRESHOLD;
    self->cooldown_ms = DEFAULT_COOLDOWN_MS;
    self->timer_id = 0;
    self->custom_probe_fn = NULL;
    self->custom_probe_data = NULL;
}

MultiWifiPathMonitor *multiwifi_path_monitor_new(void)
{
    return g_object_new(MULTIWIFI_TYPE_PATH_MONITOR, NULL);
}

void multiwifi_path_monitor_set_config(MultiWifiPathMonitor *self,
                                       const MultiWifiLoadBalanceConfig *config)
{
    if (self == NULL || config == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);
    self->interval_ms = config->health_check_interval_ms;
    self->failure_threshold = config->failure_threshold;
    self->recovery_threshold = config->recovery_threshold;
    self->cooldown_ms = config->cooldown_ms;
    g_mutex_unlock(&self->mutex);
}

void multiwifi_path_monitor_set_probe_func(MultiWifiPathMonitor *self,
                                           MultiWifiProbeFunc probe_fn,
                                           gpointer user_data)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    self->custom_probe_fn = probe_fn;
    self->custom_probe_data = user_data;
    g_mutex_unlock(&self->mutex);
}

static PathEntry *find_entry_locked(MultiWifiPathMonitor *self, const gchar *ifname)
{
    if (ifname == NULL) {
        return NULL;
    }
    for (guint i = 0; i < self->paths->len; i++) {
        PathEntry *e = g_ptr_array_index(self->paths, i);
        if (g_strcmp0(e->info->ifname, ifname) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Default unprivileged socket probe: TCP connect attempt to target on DNS (53) or HTTP (80) */
static gboolean default_socket_probe(const gchar *ifname,
                                     const gchar *source_ip,
                                     const gchar *target_ip,
                                     gdouble *out_rtt_ms,
                                     gpointer user_data)
{
    (void)ifname;
    (void)user_data;

    if (target_ip == NULL || *target_ip == '\0') {
        return FALSE;
    }

    gint64 start_time = g_get_monotonic_time();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return FALSE;
    }

    /* Set socket non-blocking */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    /* Bind to local source IP if present to pin outgoing traffic to this uplink */
    if (source_ip != NULL && *source_ip != '\0') {
        gchar *clean_ip = g_strdup(source_ip);
        gchar *slash = strchr(clean_ip, '/');
        if (slash != NULL) {
            *slash = '\0';
        }

        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, clean_ip, &local_addr.sin_addr) == 1) {
            bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));
        }
        g_free(clean_ip);
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53); /* DNS port probe */
    if (inet_pton(AF_INET, target_ip, &dest_addr.sin_addr) != 1) {
        close(sock);
        return FALSE;
    }

    int res = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    gboolean success = FALSE;

    if (res == 0) {
        success = TRUE;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 400000; /* 400ms timeout per probe */

        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel > 0) {
            int sock_err = 0;
            socklen_t len = sizeof(sock_err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0) {
                /* Even ECONNREFUSED means the gateway/host replied back! */
                if (sock_err == 0 || sock_err == ECONNREFUSED) {
                    success = TRUE;
                }
            }
        }
    }

    close(sock);

    gint64 elapsed_us = g_get_monotonic_time() - start_time;
    if (out_rtt_ms != NULL) {
        *out_rtt_ms = (gdouble)elapsed_us / 1000.0;
    }

    return success;
}

static void update_path_entry_result(MultiWifiPathMonitor *self,
                                     PathEntry *entry,
                                     gboolean success,
                                     gdouble measured_rtt)
{
    MultiWifiPathInfo *p = entry->info;
    gint64 now_us = g_get_monotonic_time();
    p->last_probe_time_us = now_us;

    /* Record rolling window */
    entry->probe_history[entry->probe_history_idx] = success ? 1 : 0;
    entry->probe_history_idx = (entry->probe_history_idx + 1) % ROLLING_WINDOW_SIZE;
    if (entry->probe_history_count < ROLLING_WINDOW_SIZE) {
        entry->probe_history_count++;
    }

    guint successful_count = 0;
    for (guint i = 0; i < entry->probe_history_count; i++) {
        if (entry->probe_history[i]) {
            successful_count++;
        }
    }
    p->packet_loss_percent = (entry->probe_history_count > 0)
        ? ((entry->probe_history_count - successful_count) * 100) / entry->probe_history_count
        : 0;

    gint64 cooldown_elapsed_ms = (now_us - p->last_state_change_us) / 1000;

    if (success) {
        p->consecutive_successes++;
        p->consecutive_failures = 0;

        /* Exponential Moving Average for RTT */
        if (p->rtt_ms <= 0.0) {
            p->rtt_ms = measured_rtt;
            p->min_rtt_ms = measured_rtt;
            p->max_rtt_ms = measured_rtt;
            p->jitter_ms = 0.0;
        } else {
            p->jitter_ms = fabs(measured_rtt - p->rtt_ms);
            p->rtt_ms = (0.7 * p->rtt_ms) + (0.3 * measured_rtt);
            if (measured_rtt < p->min_rtt_ms) p->min_rtt_ms = measured_rtt;
            if (measured_rtt > p->max_rtt_ms) p->max_rtt_ms = measured_rtt;
        }

        if (p->state == MULTIWIFI_PATH_STATE_ONLINE) {
            if (p->rtt_ms > 350.0 || p->packet_loss_percent >= 30) {
                p->state = MULTIWIFI_PATH_STATE_DEGRADED;
                p->is_eligible = FALSE;
                p->last_state_change_us = now_us;
                g_free(p->status_reason);
                p->status_reason = g_strdup_printf("Elevated latency (%.1f ms) or loss (%u%%)",
                                                   p->rtt_ms, p->packet_loss_percent);
            } else {
                g_free(p->status_reason);
                p->status_reason = g_strdup_printf("Healthy (RTT: %.1f ms, Loss: %u%%)",
                                                   p->rtt_ms, p->packet_loss_percent);
            }
        } else {
            /* State was OFFLINE, LOCAL_ONLY, or DEGRADED: check recovery threshold & cooldown */
            if (p->consecutive_successes >= self->recovery_threshold) {
                if (cooldown_elapsed_ms >= (gint64)self->cooldown_ms ||
                    p->state == MULTIWIFI_PATH_STATE_PROBING ||
                    p->state == MULTIWIFI_PATH_STATE_UNKNOWN) {
                    p->state = MULTIWIFI_PATH_STATE_ONLINE;
                    p->is_eligible = TRUE;
                    p->last_state_change_us = now_us;
                    g_free(p->status_reason);
                    p->status_reason = g_strdup("Online: Verified upstream reachability");
                } else {
                    g_free(p->status_reason);
                    p->status_reason = g_strdup_printf("Stabilizing in cooldown (%ld/%u ms)",
                                                       cooldown_elapsed_ms, self->cooldown_ms);
                }
            } else {
                g_free(p->status_reason);
                p->status_reason = g_strdup_printf("Recovering: %u/%u successful probes",
                                                   p->consecutive_successes, self->recovery_threshold);
            }
        }
    } else {
        /* Probe failed */
        p->consecutive_failures++;
        p->consecutive_successes = 0;

        if (p->state == MULTIWIFI_PATH_STATE_ONLINE) {
            if (p->consecutive_failures >= self->failure_threshold) {
                p->state = MULTIWIFI_PATH_STATE_LOCAL_ONLY;
                p->is_eligible = FALSE;
                p->last_state_change_us = now_us;
                g_free(p->status_reason);
                p->status_reason = g_strdup_printf("Unreachable: %u consecutive probe failures",
                                                   p->consecutive_failures);
            } else {
                g_free(p->status_reason);
                p->status_reason = g_strdup_printf("Degrading: %u/%u probe failures (Hysteresis)",
                                                   p->consecutive_failures, self->failure_threshold);
            }
        } else if (p->state == MULTIWIFI_PATH_STATE_DEGRADED) {
            if (p->consecutive_failures >= self->failure_threshold) {
                p->state = MULTIWIFI_PATH_STATE_LOCAL_ONLY;
                p->is_eligible = FALSE;
                p->last_state_change_us = now_us;
                g_free(p->status_reason);
                p->status_reason = g_strdup("Degraded path failed reachability threshold");
            }
        } else {
            g_free(p->status_reason);
            p->status_reason = g_strdup_printf("Gateway/Internet unreachable (failures: %u)",
                                               p->consecutive_failures);
        }
    }
}

void multiwifi_path_monitor_probe_all_now(MultiWifiPathMonitor *self)
{
    if (self == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);
    MultiWifiProbeFunc probe_fn = self->custom_probe_fn ? self->custom_probe_fn : default_socket_probe;
    gpointer probe_data = self->custom_probe_data;

    for (guint i = 0; i < self->paths->len; i++) {
        PathEntry *e = g_ptr_array_index(self->paths, i);
        MultiWifiPathInfo *p = e->info;

        /* If interface is offline or has no IP/gateway, skip active socket probe */
        if (p->state == MULTIWIFI_PATH_STATE_OFFLINE || p->gateway == NULL || p->source_ip == NULL) {
            continue;
        }

        gdouble measured_rtt = 0.0;
        gboolean success = probe_fn(p->ifname, p->source_ip, p->gateway, &measured_rtt, probe_data);
        update_path_entry_result(self, e, success, measured_rtt);
    }
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_PATHS_CHANGED], 0);
}

static gboolean on_timer_tick(gpointer user_data)
{
    MultiWifiPathMonitor *self = MULTIWIFI_PATH_MONITOR(user_data);
    multiwifi_path_monitor_probe_all_now(self);
    return G_SOURCE_CONTINUE;
}

void multiwifi_path_monitor_start(MultiWifiPathMonitor *self)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    if (self->timer_id == 0) {
        self->timer_id = g_timeout_add(self->interval_ms, on_timer_tick, self);
    }
    g_mutex_unlock(&self->mutex);

    /* Run one immediate probe round */
    multiwifi_path_monitor_probe_all_now(self);
}

void multiwifi_path_monitor_stop(MultiWifiPathMonitor *self)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    if (self->timer_id > 0) {
        g_source_remove(self->timer_id);
        self->timer_id = 0;
    }
    g_mutex_unlock(&self->mutex);
}

void multiwifi_path_monitor_update_device(MultiWifiPathMonitor *self,
                                          const MultiWifiDeviceInfo *device)
{
    if (self == NULL || device == NULL || device->ifname == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);
    PathEntry *entry = find_entry_locked(self, device->ifname);
    if (entry == NULL) {
        entry = path_entry_new(device->ifname);
        g_ptr_array_add(self->paths, entry);
    }

    MultiWifiPathInfo *p = entry->info;
    g_free(p->gateway);
    p->gateway = g_strdup(device->ipv4_gateway);

    g_free(p->source_ip);
    p->source_ip = g_strdup(device->ipv4_address);

    if (device->state != MULTIWIFI_DEVICE_STATE_CONNECTED) {
        p->state = MULTIWIFI_PATH_STATE_OFFLINE;
        p->is_eligible = FALSE;
        p->consecutive_successes = 0;
        p->consecutive_failures = 0;
        g_free(p->status_reason);
        p->status_reason = g_strdup(multiwifi_device_state_to_string(device->state));
    } else if (p->state == MULTIWIFI_PATH_STATE_OFFLINE || p->state == MULTIWIFI_PATH_STATE_UNKNOWN) {
        p->state = MULTIWIFI_PATH_STATE_PROBING;
        g_free(p->status_reason);
        p->status_reason = g_strdup("Connected locally; initiating health probes");
    }

    g_mutex_unlock(&self->mutex);
    g_signal_emit(self, s_signals[SIGNAL_PATHS_CHANGED], 0);
}

void multiwifi_path_monitor_remove_device(MultiWifiPathMonitor *self,
                                          const gchar *ifname)
{
    if (self == NULL || ifname == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);
    for (guint i = 0; i < self->paths->len; i++) {
        PathEntry *e = g_ptr_array_index(self->paths, i);
        if (g_strcmp0(e->info->ifname, ifname) == 0) {
            g_ptr_array_remove_index(self->paths, i);
            break;
        }
    }
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_PATHS_CHANGED], 0);
}

GPtrArray *multiwifi_path_monitor_dup_paths(MultiWifiPathMonitor *self)
{
    if (self == NULL) {
        return NULL;
    }

    g_mutex_lock(&self->mutex);
    GPtrArray *dup = g_ptr_array_new_full(self->paths->len, (GDestroyNotify)multiwifi_path_info_free);
    for (guint i = 0; i < self->paths->len; i++) {
        PathEntry *e = g_ptr_array_index(self->paths, i);
        g_ptr_array_add(dup, multiwifi_path_info_copy(e->info));
    }
    g_mutex_unlock(&self->mutex);

    return dup;
}

MultiWifiPathInfo *multiwifi_path_monitor_find_path(MultiWifiPathMonitor *self,
                                                    const gchar *ifname)
{
    if (self == NULL || ifname == NULL) {
        return NULL;
    }

    MultiWifiPathInfo *found = NULL;
    g_mutex_lock(&self->mutex);
    PathEntry *entry = find_entry_locked(self, ifname);
    if (entry != NULL) {
        found = multiwifi_path_info_copy(entry->info);
    }
    g_mutex_unlock(&self->mutex);

    return found;
}
