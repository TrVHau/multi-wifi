#ifndef MULTIWIFI_NETLINK_ROUTE_H
#define MULTIWIFI_NETLINK_ROUTE_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * multiwifi_netlink_open:
 * @error: (out)(optional): Return location for GError
 *
 * Opens and binds a new NETLINK_ROUTE socket.
 *
 * Returns: Socket file descriptor, or -1 on error.
 */
int multiwifi_netlink_open(GError **error);

/**
 * multiwifi_netlink_close:
 * @nl_fd: Netlink socket file descriptor
 */
void multiwifi_netlink_close(int nl_fd);

/**
 * multiwifi_netlink_add_rule:
 * @nl_fd: Open netlink socket
 * @table_id: Routing table ID (e.g. 10001)
 * @pref: Rule priority preference (e.g. 20001)
 * @fwmark: Fwmark to match (or 0)
 * @fwmask: Fwmask to apply (or 0)
 * @src_ip: (nullable): IPv4 source address string or %NULL
 * @error: (out)(optional): Return location for GError
 *
 * Adds a routing policy database rule (fib_rule) via RTM_NEWRULE.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_netlink_add_rule(int nl_fd,
                                    uint32_t table_id,
                                    uint32_t pref,
                                    uint32_t fwmark,
                                    uint32_t fwmask,
                                    const gchar *src_ip,
                                    GError **error);

/**
 * multiwifi_netlink_del_rule:
 * @nl_fd: Open netlink socket
 * @table_id: Routing table ID
 * @pref: Rule priority preference
 * @fwmark: Fwmark
 * @fwmask: Fwmask
 * @src_ip: (nullable): IPv4 source address string or %NULL
 * @error: (out)(optional): Return location for GError
 *
 * Deletes a routing policy database rule via RTM_DELRULE.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_netlink_del_rule(int nl_fd,
                                    uint32_t table_id,
                                    uint32_t pref,
                                    uint32_t fwmark,
                                    uint32_t fwmask,
                                    const gchar *src_ip,
                                    GError **error);

/**
 * multiwifi_netlink_verify_rule:
 * @nl_fd: Open netlink socket
 * @table_id: Target table ID
 * @pref: Rule priority preference
 * @exists: (out): Set to %TRUE if rule is found in kernel RPDB
 * @error: (out)(optional): Return location for GError
 *
 * Queries kernel RPDB to verify if rule exists.
 *
 * Returns: %TRUE if query completed without transport error, %FALSE otherwise.
 */
gboolean multiwifi_netlink_verify_rule(int nl_fd,
                                       uint32_t table_id,
                                       uint32_t pref,
                                       gboolean *exists,
                                       GError **error);

/**
 * multiwifi_netlink_add_route:
 * @nl_fd: Open netlink socket
 * @table_id: Target routing table
 * @gateway: IPv4 gateway string (e.g. "192.168.1.1")
 * @ifname: Interface name (e.g. "wlan0")
 * @metric: Route metric priority
 * @error: (out)(optional): Return location for GError
 *
 * Adds default route to designated table via RTM_NEWROUTE.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_netlink_add_route(int nl_fd,
                                     uint32_t table_id,
                                     const gchar *gateway,
                                     const gchar *ifname,
                                     uint32_t metric,
                                     GError **error);

/**
 * multiwifi_netlink_del_route:
 * @nl_fd: Open netlink socket
 * @table_id: Target routing table
 * @gateway: IPv4 gateway string
 * @ifname: Interface name
 * @error: (out)(optional): Return location for GError
 *
 * Deletes route from designated table via RTM_DELROUTE.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_netlink_del_route(int nl_fd,
                                     uint32_t table_id,
                                     const gchar *gateway,
                                     const gchar *ifname,
                                     GError **error);

/**
 * multiwifi_netlink_verify_route:
 * @nl_fd: Open netlink socket
 * @table_id: Target routing table
 * @gateway: Expected gateway string
 * @ifname: Expected interface name
 * @exists: (out): Set to %TRUE if route exists
 * @error: (out)(optional): Return location for GError
 *
 * Reads back kernel routing table to verify route.
 *
 * Returns: %TRUE if query completed without transport error, %FALSE otherwise.
 */
gboolean multiwifi_netlink_verify_route(int nl_fd,
                                        uint32_t table_id,
                                        const gchar *gateway,
                                        const gchar *ifname,
                                        gboolean *exists,
                                        GError **error);

/**
 * multiwifi_netlink_table_has_routes:
 * @nl_fd: Open netlink socket
 * @table_id: Routing table to check
 * @has_routes: (out): Set to %TRUE if table contains any routes
 * @error: (out)(optional): Return location for GError
 *
 * Checks if target routing table already has active routes (conflict detection).
 *
 * Returns: %TRUE if check succeeded, %FALSE on error.
 */
gboolean multiwifi_netlink_table_has_routes(int nl_fd,
                                            uint32_t table_id,
                                            gboolean *has_routes,
                                            GError **error);

G_END_DECLS

#endif /* MULTIWIFI_NETLINK_ROUTE_H */
