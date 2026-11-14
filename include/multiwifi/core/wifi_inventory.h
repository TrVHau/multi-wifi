#ifndef MULTIWIFI_WIFI_INVENTORY_H
#define MULTIWIFI_WIFI_INVENTORY_H

#include <glib-object.h>
#include "multiwifi/core/wifi_device.h"

G_BEGIN_DECLS

#define MULTIWIFI_TYPE_INVENTORY (multiwifi_inventory_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiInventory, multiwifi_inventory, MULTIWIFI, INVENTORY, GObject)

/**
 * multiwifi_inventory_new:
 *
 * Creates a new Wi-Fi inventory monitor.
 *
 * Returns: (transfer full): A new MultiWifiInventory instance
 */
MultiWifiInventory *multiwifi_inventory_new(void);

/**
 * multiwifi_inventory_start:
 * @self: MultiWifiInventory instance
 * @error: (out)(optional): Return location for GError
 *
 * Connects to NetworkManager via libnm asynchronously and starts tracking Wi-Fi
 * devices. Safe to call from unprivileged user process.
 *
 * Returns: %TRUE if initialization succeeded, %FALSE on error.
 */
gboolean multiwifi_inventory_start(MultiWifiInventory *self, GError **error);

/**
 * multiwifi_inventory_is_running:
 * @self: MultiWifiInventory instance
 *
 * Returns: %TRUE if NetworkManager daemon is actively reachable, %FALSE otherwise.
 */
gboolean multiwifi_inventory_is_running(MultiWifiInventory *self);

/**
 * multiwifi_inventory_dup_devices:
 * @self: MultiWifiInventory instance
 *
 * Returns a newly allocated GPtrArray of deep-copied MultiWifiDeviceInfo pointers.
 *
 * Returns: (transfer full)(element-type MultiWifiDeviceInfo): Array of device info structs,
 * free with g_ptr_array_unref().
 */
GPtrArray *multiwifi_inventory_dup_devices(MultiWifiInventory *self);

/**
 * multiwifi_inventory_find_device:
 * @self: MultiWifiInventory instance
 * @ifname: Interface name, e.g. "wlp0s20f0u1"
 *
 * Returns: (transfer full)(nullable): Deep copy of device info for @ifname,
 * or %NULL if not found. Free with multiwifi_device_info_free().
 */
MultiWifiDeviceInfo *multiwifi_inventory_find_device(MultiWifiInventory *self, const gchar *ifname);

G_END_DECLS

#endif /* MULTIWIFI_WIFI_INVENTORY_H */
