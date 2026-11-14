#ifndef MULTIWIFI_LOOP_PREVENTION_H
#define MULTIWIFI_LOOP_PREVENTION_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct _MultiWifiLoopPrevention MultiWifiLoopPrevention;

/**
 * multiwifi_loop_prevention_new:
 * @server_ip: Destination server IP to route directly
 * @physical_gw: Physical gateway IP
 * @physical_ifname: Physical outgoing interface name
 * @metric: Metric priority (e.g. 10)
 *
 * Returns: (transfer full): Newly created MultiWifiLoopPrevention instance.
 */
MultiWifiLoopPrevention *multiwifi_loop_prevention_new(const gchar *server_ip,
                                                       const gchar *physical_gw,
                                                       const gchar *physical_ifname,
                                                       uint32_t metric);

/**
 * multiwifi_loop_prevention_free:
 * @self: (transfer full)(nullable): Instance to free
 */
void multiwifi_loop_prevention_free(MultiWifiLoopPrevention *self);

/**
 * multiwifi_loop_prevention_apply:
 * @self: Loop prevention manager
 * @error: (out)(optional): Return location for GError
 *
 * Installs explicit /32 host route via physical gateway to prevent tunnel loops.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_loop_prevention_apply(MultiWifiLoopPrevention *self, GError **error);

/**
 * multiwifi_loop_prevention_rollback:
 * @self: Loop prevention manager
 * @error: (out)(optional): Return location for GError
 *
 * Removes the installed host route.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_loop_prevention_rollback(MultiWifiLoopPrevention *self, GError **error);

/**
 * multiwifi_loop_prevention_is_active:
 * @self: Loop prevention manager
 *
 * Returns: %TRUE if host route is currently active.
 */
gboolean multiwifi_loop_prevention_is_active(MultiWifiLoopPrevention *self);

G_END_DECLS

#endif /* MULTIWIFI_LOOP_PREVENTION_H */
