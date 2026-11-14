#ifndef MULTIWIFI_BONDING_CLIENT_H
#define MULTIWIFI_BONDING_CLIENT_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include "multiwifi/core/bonding_protocol.h"
#include "multiwifi/core/tun_device.h"
#include "multiwifi/core/loop_prevention.h"

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_BONDING_STATE_DISCONNECTED = 0,
    MULTIWIFI_BONDING_STATE_CONNECTING,
    MULTIWIFI_BONDING_STATE_AUTHENTICATING,
    MULTIWIFI_BONDING_STATE_CONNECTED,
    MULTIWIFI_BONDING_STATE_DISCONNECTING,
    MULTIWIFI_BONDING_STATE_FAILED
} MultiWifiBondingState;

typedef struct _MultiWifiBondingClient MultiWifiBondingClient;

/**
 * multiwifi_bonding_client_new:
 *
 * Allocates a new MultiWifiBondingClient instance.
 *
 * Returns: (transfer full): Newly created client.
 */
MultiWifiBondingClient *multiwifi_bonding_client_new(void);

/**
 * multiwifi_bonding_client_free:
 * @self: (transfer full)(nullable): Client to free
 */
void multiwifi_bonding_client_free(MultiWifiBondingClient *self);

/**
 * multiwifi_bonding_client_set_profile:
 * @self: Client instance
 * @profile_id: Profile ID string
 * @server_host: Server IP or hostname
 * @server_port: Server TCP/MPTCP port
 * @auth_token: Secret authentication token
 */
void multiwifi_bonding_client_set_profile(MultiWifiBondingClient *self,
                                          const gchar *profile_id,
                                          const gchar *server_host,
                                          uint16_t server_port,
                                          const gchar *auth_token);

/**
 * multiwifi_bonding_client_set_uplink:
 * @self: Client instance
 * @physical_ifname: Physical Wi-Fi interface name
 * @physical_gw: Physical default gateway
 */
void multiwifi_bonding_client_set_uplink(MultiWifiBondingClient *self,
                                         const gchar *physical_ifname,
                                         const gchar *physical_gw);

/**
 * multiwifi_bonding_client_set_tun_device:
 * @self: Client instance
 * @tun: (transfer full)(nullable): Pre-configured or mock TUN device
 */
void multiwifi_bonding_client_set_tun_device(MultiWifiBondingClient *self,
                                             MultiWifiTunDevice *tun);

/**
 * multiwifi_bonding_client_start:
 * @self: Client instance
 * @error: (out)(optional): Return location for GError
 *
 * Connects to server, performs auth handshake, configures TUN and starts forwarding.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_bonding_client_start(MultiWifiBondingClient *self, GError **error);

/**
 * multiwifi_bonding_client_stop:
 * @self: Client instance
 *
 * Gracefully shuts down forwarding, closes tunnel and restores routes.
 */
void multiwifi_bonding_client_stop(MultiWifiBondingClient *self);

/**
 * multiwifi_bonding_client_get_state:
 * @self: Client instance
 *
 * Returns: Current state enum.
 */
MultiWifiBondingState multiwifi_bonding_client_get_state(MultiWifiBondingClient *self);

/**
 * multiwifi_bonding_client_get_virtual_ip:
 * @self: Client instance
 *
 * Returns: Assigned virtual IP address string or %NULL.
 */
const gchar *multiwifi_bonding_client_get_virtual_ip(MultiWifiBondingClient *self);

/**
 * multiwifi_bonding_client_is_mptcp:
 * @self: Client instance
 *
 * Returns: %TRUE if transport is running on MPTCP socket, %FALSE if standard TCP fallback.
 */
gboolean multiwifi_bonding_client_is_mptcp(MultiWifiBondingClient *self);

/**
 * multiwifi_bonding_client_state_to_string:
 * @state: Bonding state enum
 *
 * Returns: Human-readable string representation.
 */
const gchar *multiwifi_bonding_client_state_to_string(MultiWifiBondingState state);

G_END_DECLS

#endif /* MULTIWIFI_BONDING_CLIENT_H */
