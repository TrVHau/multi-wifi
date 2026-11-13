#ifndef MULTIWIFI_CAPABILITIES_H
#define MULTIWIFI_CAPABILITIES_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_CAP_STATE_UNSUPPORTED = 0,
    MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE,
    MULTIWIFI_CAP_STATE_ACTIVE,
    MULTIWIFI_CAP_STATE_UNKNOWN
} MultiWifiCapState;

typedef struct {
    MultiWifiCapState kernel_mptcp;
    MultiWifiCapState nftables;
    MultiWifiCapState tun_device;
    MultiWifiCapState network_manager;
    gchar *mptcp_details;
    gchar *kernel_version;
} MultiWifiCapabilities;

/**
 * multiwifi_capabilities_new:
 *
 * Allocates and initializes an empty capability structure.
 *
 * Returns: (transfer full): Newly allocated MultiWifiCapabilities, free with
 * multiwifi_capabilities_free().
 */
MultiWifiCapabilities *multiwifi_capabilities_new(void);

/**
 * multiwifi_capabilities_probe:
 * @caps: (transfer none): Capabilities structure to populate
 *
 * Probes the local system environment for networking capabilities in a strictly
 * non-mutating, unprivileged manner.
 */
void multiwifi_capabilities_probe(MultiWifiCapabilities *caps);

/**
 * multiwifi_cap_state_to_string:
 * @state: Capability state
 *
 * Returns: (transfer none): Human-readable string for state.
 */
const gchar *multiwifi_cap_state_to_string(MultiWifiCapState state);

/**
 * multiwifi_capabilities_free:
 * @caps: (transfer full)(nullable): Capabilities structure to free
 */
void multiwifi_capabilities_free(MultiWifiCapabilities *caps);

G_END_DECLS

#endif /* MULTIWIFI_CAPABILITIES_H */
