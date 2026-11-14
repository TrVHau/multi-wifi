#include "multiwifi/core/loop_prevention.h"
#include "multiwifi/core/netlink_route.h"
#include <gio/gio.h>
#include <string.h>

struct _MultiWifiLoopPrevention {
    gchar *server_ip;
    gchar *physical_gw;
    gchar *physical_ifname;
    uint32_t metric;
    gboolean is_active;
    GMutex mutex;
};

MultiWifiLoopPrevention *multiwifi_loop_prevention_new(const gchar *server_ip,
                                                       const gchar *physical_gw,
                                                       const gchar *physical_ifname,
                                                       uint32_t metric)
{
    MultiWifiLoopPrevention *self = g_new0(MultiWifiLoopPrevention, 1);
    self->server_ip = g_strdup(server_ip);
    self->physical_gw = g_strdup(physical_gw);
    self->physical_ifname = g_strdup(physical_ifname);
    self->metric = (metric > 0) ? metric : 10;
    self->is_active = FALSE;
    g_mutex_init(&self->mutex);
    return self;
}

void multiwifi_loop_prevention_free(MultiWifiLoopPrevention *self)
{
    if (self == NULL) {
        return;
    }
    if (self->is_active) {
        multiwifi_loop_prevention_rollback(self, NULL);
    }
    g_mutex_clear(&self->mutex);
    g_free(self->server_ip);
    g_free(self->physical_gw);
    g_free(self->physical_ifname);
    g_free(self);
}

gboolean multiwifi_loop_prevention_apply(MultiWifiLoopPrevention *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Loop prevention manager is NULL");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    if (self->server_ip == NULL || self->physical_gw == NULL || self->physical_ifname == NULL) {
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Incomplete loop prevention configuration");
        return FALSE;
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd < 0) {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    /* Main table ID = 254 */
    gboolean ok = multiwifi_netlink_add_host_route(
        nl_fd,
        254,
        self->server_ip,
        self->physical_gw,
        self->physical_ifname,
        self->metric,
        error
    );

    multiwifi_netlink_close(nl_fd);

    if (ok) {
        self->is_active = TRUE;
    }

    g_mutex_unlock(&self->mutex);
    return ok;
}

gboolean multiwifi_loop_prevention_rollback(MultiWifiLoopPrevention *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Loop prevention manager is NULL");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    if (!self->is_active) {
        g_mutex_unlock(&self->mutex);
        return TRUE;
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd < 0) {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    gboolean ok = multiwifi_netlink_del_host_route(
        nl_fd,
        254,
        self->server_ip,
        self->physical_gw,
        self->physical_ifname,
        error
    );

    multiwifi_netlink_close(nl_fd);

    self->is_active = FALSE;
    g_mutex_unlock(&self->mutex);
    return ok;
}

gboolean multiwifi_loop_prevention_is_active(MultiWifiLoopPrevention *self)
{
    if (self == NULL) {
        return FALSE;
    }
    gboolean active;
    g_mutex_lock(&self->mutex);
    active = self->is_active;
    g_mutex_unlock(&self->mutex);
    return active;
}
