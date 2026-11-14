#ifndef MULTIWIFI_TUN_DEVICE_H
#define MULTIWIFI_TUN_DEVICE_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

#define MULTIWIFI_DEFAULT_TUN_NAME "mwtun0"
#define MULTIWIFI_DEFAULT_TUN_MTU  1420

typedef struct _MultiWifiTunDevice MultiWifiTunDevice;

/**
 * multiwifi_tun_device_new:
 * @name: Desired interface name (e.g. "mwtun0") or %NULL for default
 * @mtu: MTU in bytes (e.g. 1420) or 0 for default
 *
 * Returns: (transfer full): Newly allocated MultiWifiTunDevice.
 */
MultiWifiTunDevice *multiwifi_tun_device_new(const gchar *name, uint32_t mtu);

/**
 * multiwifi_tun_device_free:
 * @self: (transfer full)(nullable): TUN device to free
 */
void multiwifi_tun_device_free(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_set_mock_fd:
 * @self: TUN device
 * @fd: File descriptor (e.g. from socketpair) for testing
 */
void multiwifi_tun_device_set_mock_fd(MultiWifiTunDevice *self, int fd);

/**
 * multiwifi_tun_device_open:
 * @self: TUN device
 * @error: (out)(optional): Return location for GError
 *
 * Opens /dev/net/tun and configures interface name and non-blocking flags.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_tun_device_open(MultiWifiTunDevice *self, GError **error);

/**
 * multiwifi_tun_device_close:
 * @self: TUN device
 *
 * Closes TUN file descriptor and releases kernel interface.
 */
void multiwifi_tun_device_close(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_is_open:
 * @self: TUN device
 *
 * Returns: %TRUE if device is currently open.
 */
gboolean multiwifi_tun_device_is_open(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_get_fd:
 * @self: TUN device
 *
 * Returns: Underlying file descriptor, or -1 if closed.
 */
int multiwifi_tun_device_get_fd(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_get_name:
 * @self: TUN device
 *
 * Returns: Actual kernel interface name (e.g. "mwtun0").
 */
const gchar *multiwifi_tun_device_get_name(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_get_mtu:
 * @self: TUN device
 *
 * Returns: Configured MTU.
 */
uint32_t multiwifi_tun_device_get_mtu(MultiWifiTunDevice *self);

/**
 * multiwifi_tun_device_configure_ip:
 * @self: TUN device
 * @ipv4_addr: Virtual IP address (e.g. "10.8.0.2")
 * @netmask: Subnet mask (e.g. "255.255.255.0")
 * @error: (out)(optional): Return location for GError
 *
 * Configures IP address and brings interface UP.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_tun_device_configure_ip(MultiWifiTunDevice *self,
                                           const gchar *ipv4_addr,
                                           const gchar *netmask,
                                           GError **error);

/**
 * multiwifi_tun_device_read:
 * @self: TUN device
 * @buf: Output buffer
 * @max_len: Max bytes to read
 * @error: (out)(optional): Return location for GError
 *
 * Returns: Number of bytes read, 0 if EAGAIN/EWOULDBLOCK, or -1 on error.
 */
gssize multiwifi_tun_device_read(MultiWifiTunDevice *self,
                                 guint8 *buf,
                                 gsize max_len,
                                 GError **error);

/**
 * multiwifi_tun_device_write:
 * @self: TUN device
 * @buf: Packet buffer
 * @len: Number of bytes to write
 * @error: (out)(optional): Return location for GError
 *
 * Returns: Number of bytes written, or -1 on error.
 */
gssize multiwifi_tun_device_write(MultiWifiTunDevice *self,
                                  const guint8 *buf,
                                  gsize len,
                                  GError **error);

G_END_DECLS

#endif /* MULTIWIFI_TUN_DEVICE_H */
