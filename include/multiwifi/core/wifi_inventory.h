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
 * multiwifi_inventory_refresh:
 * @self: MultiWifiInventory instance
 *
 * Forces an immediate synchronous refresh of the cached device inventory.
 */
void multiwifi_inventory_refresh(MultiWifiInventory *self);

/**
 * multiwifi_inventory_find_device:
 * @self: MultiWifiInventory instance
 * @ifname: Interface name, e.g. "wlp0s20f0u1"
 *
 * Returns: (transfer full)(nullable): Deep copy of device info for @ifname,
 * or %NULL if not found. Free with multiwifi_device_info_free().
 */
MultiWifiDeviceInfo *multiwifi_inventory_find_device(MultiWifiInventory *self, const gchar *ifname);

/**
 * multiwifi_inventory_dup_access_points:
 * @self: MultiWifiInventory instance
 * @ifname: Interface name, e.g. "wlp0s20f0u1"
 *
 * Returns a newly allocated GPtrArray of deep-copied MultiWifiAccessPointInfo pointers
 * visible to @ifname, deduplicated and sorted by signal strength.
 *
 * Returns: (transfer full)(element-type MultiWifiAccessPointInfo): Array of AP info structs,
 * free with g_ptr_array_unref().
 */
GPtrArray *multiwifi_inventory_dup_access_points(MultiWifiInventory *self, const gchar *ifname);

/**
 * multiwifi_inventory_request_scan_async:
 * @self: MultiWifiInventory instance
 * @ifname: Interface name to trigger scan on
 * @cancellable: Optional GCancellable
 * @callback: Callback when scan finishes
 * @user_data: User data
 */
void multiwifi_inventory_request_scan_async(MultiWifiInventory *self,
                                            const gchar *ifname,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean multiwifi_inventory_request_scan_finish(MultiWifiInventory *self,
                                                 GAsyncResult *res,
                                                 GError **error);

/**
 * multiwifi_inventory_connect_wifi_async:
 * @self: MultiWifiInventory instance
 * @ifname: Interface name to connect, e.g. "wlp0s20f0u1"
 * @ssid: Target SSID name
 * @password: (nullable): Wi-Fi PSK passphrase if not saved
 * @cancellable: Optional GCancellable
 * @callback: Callback when connection attempt finishes
 * @user_data: User data
 */
void multiwifi_inventory_connect_wifi_async(MultiWifiInventory *self,
                                            const gchar *ifname,
                                            const gchar *ssid,
                                            const gchar *password,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean multiwifi_inventory_connect_wifi_finish(MultiWifiInventory *self,
                                                 GAsyncResult *res,
                                                 GError **error);

G_END_DECLS

#endif /* MULTIWIFI_WIFI_INVENTORY_H */
