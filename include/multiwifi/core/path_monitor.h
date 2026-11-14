#ifndef MULTIWIFI_PATH_MONITOR_H
#define MULTIWIFI_PATH_MONITOR_H

#include <glib-object.h>
#include "multiwifi/core/wifi_device.h"
#include "multiwifi/core/config.h"

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_PATH_STATE_UNKNOWN = 0,
    MULTIWIFI_PATH_STATE_OFFLINE,       /* Device down, disconnected, or unmanaged */
    MULTIWIFI_PATH_STATE_LOCAL_ONLY,    /* Local association/IP active, but upstream unreachable */
    MULTIWIFI_PATH_STATE_PROBING,       /* Active probing in-flight */
    MULTIWIFI_PATH_STATE_ONLINE,        /* Fully healthy and verified reachable */
    MULTIWIFI_PATH_STATE_DEGRADED       /* Degraded path (high RTT or elevated packet loss) */
} MultiWifiPathState;

typedef struct {
    gchar *ifname;                  /* Interface name, e.g. "wlp0s20f0u1" (owned) */
    gchar *gateway;                 /* Current default gateway or NULL (owned) */
    gchar *source_ip;               /* Bound source IP address or NULL (owned) */
    MultiWifiPathState state;       /* Current path reachability state */
    gchar *status_reason;           /* Diagnostic reason for current state (owned) */
    gdouble rtt_ms;                 /* Smoothed round-trip time in milliseconds */
    gdouble min_rtt_ms;             /* Minimum observed RTT */
    gdouble max_rtt_ms;             /* Maximum observed RTT */
    gdouble jitter_ms;              /* Estimated jitter */
    guint packet_loss_percent;      /* Packet loss percentage over rolling window */
    guint consecutive_successes;    /* Current streak of successful probes */
    guint consecutive_failures;     /* Current streak of failed probes */
    gint64 last_probe_time_us;      /* Monotonic timestamp of last probe */
    gint64 last_state_change_us;    /* Monotonic timestamp of last state transition */
    gboolean is_eligible;           /* Whether path is eligible for traffic assignment */
} MultiWifiPathInfo;

/**
 * multiwifi_path_info_new:
 * @ifname: Interface name
 *
 * Allocates and initializes an empty path info structure.
 *
 * Returns: (transfer full): Newly allocated MultiWifiPathInfo, free with
 * multiwifi_path_info_free().
 */
MultiWifiPathInfo *multiwifi_path_info_new(const gchar *ifname);

/**
 * multiwifi_path_info_copy:
 * @src: (transfer none): Source struct to duplicate
 *
 * Returns: (transfer full): A deep copy of @src.
 */
MultiWifiPathInfo *multiwifi_path_info_copy(const MultiWifiPathInfo *src);

/**
 * multiwifi_path_info_free:
 * @info: (transfer full)(nullable): Struct to free
 */
void multiwifi_path_info_free(MultiWifiPathInfo *info);

/**
 * multiwifi_path_state_to_string:
 * @state: Path state enum
 *
 * Returns: (transfer none): Static human-readable state string.
 */
const gchar *multiwifi_path_state_to_string(MultiWifiPathState state);

#define MULTIWIFI_TYPE_PATH_MONITOR (multiwifi_path_monitor_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiPathMonitor, multiwifi_path_monitor, MULTIWIFI, PATH_MONITOR, GObject)

/**
 * MultiWifiProbeFunc:
 * @ifname: Interface name to probe
 * @source_ip: Source IP to bind probe socket to
 * @target_ip: Gateway or target host to reach
 * @out_rtt_ms: (out): Measured RTT in milliseconds
 * @user_data: User data pointer
 *
 * Custom probe function callback for unit testing or custom transport probing.
 * Returns %TRUE if probe succeeded, %FALSE on failure.
 */
typedef gboolean (*MultiWifiProbeFunc)(const gchar *ifname,
                                       const gchar *source_ip,
                                       const gchar *target_ip,
                                       gdouble *out_rtt_ms,
                                       gpointer user_data);

/**
 * multiwifi_path_monitor_new:
 *
 * Creates a new MultiWifiPathMonitor instance with default configuration.
 *
 * Returns: (transfer full): A new MultiWifiPathMonitor object.
 */
MultiWifiPathMonitor *multiwifi_path_monitor_new(void);

/**
 * multiwifi_path_monitor_set_config:
 * @self: Path monitor instance
 * @config: Load balance config settings (thresholds, interval, cooldown)
 */
void multiwifi_path_monitor_set_config(MultiWifiPathMonitor *self,
                                       const MultiWifiLoadBalanceConfig *config);

/**
 * multiwifi_path_monitor_set_probe_func:
 * @self: Path monitor instance
 * @probe_fn: Custom probe function (or %NULL for default socket probe)
 * @user_data: User data for @probe_fn
 */
void multiwifi_path_monitor_set_probe_func(MultiWifiPathMonitor *self,
                                           MultiWifiProbeFunc probe_fn,
                                           gpointer user_data);

/**
 * multiwifi_path_monitor_update_device:
 * @self: Path monitor instance
 * @device: (transfer none): Updated device information from inventory
 *
 * Informs the monitor of device inventory changes (association, IP, disconnect).
 */
void multiwifi_path_monitor_update_device(MultiWifiPathMonitor *self,
                                          const MultiWifiDeviceInfo *device);

/**
 * multiwifi_path_monitor_remove_device:
 * @self: Path monitor instance
 * @ifname: Interface name that disappeared
 */
void multiwifi_path_monitor_remove_device(MultiWifiPathMonitor *self,
                                          const gchar *ifname);

/**
 * multiwifi_path_monitor_probe_all_now:
 * @self: Path monitor instance
 *
 * Executes one round of probing across all active paths synchronously or immediately.
 */
void multiwifi_path_monitor_probe_all_now(MultiWifiPathMonitor *self);

/**
 * multiwifi_path_monitor_start:
 * @self: Path monitor instance
 *
 * Starts the periodic probing timer using the configured interval.
 */
void multiwifi_path_monitor_start(MultiWifiPathMonitor *self);

/**
 * multiwifi_path_monitor_stop:
 * @self: Path monitor instance
 *
 * Stops the periodic probing timer.
 */
void multiwifi_path_monitor_stop(MultiWifiPathMonitor *self);

/**
 * multiwifi_path_monitor_dup_paths:
 * @self: Path monitor instance
 *
 * Returns: (transfer full)(element-type MultiWifiPathInfo): Array of deep-copied path infos,
 * free with g_ptr_array_unref().
 */
GPtrArray *multiwifi_path_monitor_dup_paths(MultiWifiPathMonitor *self);

/**
 * multiwifi_path_monitor_find_path:
 * @self: Path monitor instance
 * @ifname: Interface name to find
 *
 * Returns: (transfer full)(nullable): Deep copy of path info or %NULL.
 */
MultiWifiPathInfo *multiwifi_path_monitor_find_path(MultiWifiPathMonitor *self,
                                                    const gchar *ifname);

G_END_DECLS

#endif /* MULTIWIFI_PATH_MONITOR_H */
