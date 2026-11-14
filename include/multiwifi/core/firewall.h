#ifndef MULTIWIFI_FIREWALL_H
#define MULTIWIFI_FIREWALL_H

#include <glib-object.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

#define MULTIWIFI_NFT_TABLE_NAME "multiwifi"

#define MULTIWIFI_TYPE_FIREWALL (multiwifi_firewall_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiFirewall, multiwifi_firewall, MULTIWIFI, FIREWALL, GObject)

/**
 * multiwifi_firewall_new:
 *
 * Creates a new firewall manager utilizing libnftables.
 *
 * Returns: (transfer full): A new MultiWifiFirewall object.
 */
MultiWifiFirewall *multiwifi_firewall_new(void);

/**
 * multiwifi_firewall_apply:
 * @self: Firewall manager
 * @marks: GArray of uint32_t fwmarks representing the distribution
 * @error: (out)(optional): Return location for GError
 *
 * Applies declarative nftables rules in table 'multiwifi' to assign fwmarks
 * with flow affinity for multi-path routing.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_firewall_apply(MultiWifiFirewall *self,
                                  GArray *marks,
                                  GError **error);

/**
 * multiwifi_firewall_teardown:
 * @self: Firewall manager
 * @error: (out)(optional): Return location for GError
 *
 * Completely dismantles table 'multiwifi', cleanly restoring firewall state
 * without flushing global or unrelated rulesets.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_firewall_teardown(MultiWifiFirewall *self,
                                     GError **error);

/**
 * multiwifi_firewall_verify:
 * @self: Firewall manager
 * @exists: (out): Set to %TRUE if MultiWiFi table is active in kernel
 * @error: (out)(optional): Return location for GError
 *
 * Queries kernel nftables ruleset to verify if table 'multiwifi' exists.
 *
 * Returns: %TRUE on query success, %FALSE on error.
 */
gboolean multiwifi_firewall_verify(MultiWifiFirewall *self,
                                   gboolean *exists,
                                   GError **error);

/**
 * multiwifi_firewall_is_active:
 * @self: Firewall manager
 *
 * Returns: %TRUE if MultiWiFi firewall table is currently active.
 */
gboolean multiwifi_firewall_is_active(MultiWifiFirewall *self);

G_END_DECLS

#endif /* MULTIWIFI_FIREWALL_H */
