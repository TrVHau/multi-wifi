#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/core/logging.h"
#include <NetworkManager.h>
#include <net/if.h>

enum {
    SIGNAL_DEVICES_CHANGED,
    LAST_SIGNAL
};

static guint s_signals[LAST_SIGNAL] = { 0 };

struct _MultiWifiInventory {
    GObject parent_instance;
    NMClient *client;
    GPtrArray *cached_devices; /* MultiWifiDeviceInfo* */
    GMutex mutex;
};

G_DEFINE_TYPE(MultiWifiInventory, multiwifi_inventory, G_TYPE_OBJECT)

static void multiwifi_inventory_refresh_locked(MultiWifiInventory *self);

static void multiwifi_inventory_dispose(GObject *object)
{
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(object);

    g_mutex_lock(&self->mutex);
    if (self->cached_devices != NULL) {
        g_ptr_array_unref(self->cached_devices);
        self->cached_devices = NULL;
    }
    g_mutex_unlock(&self->mutex);

    g_clear_object(&self->client);

    G_OBJECT_CLASS(multiwifi_inventory_parent_class)->dispose(object);
}

static void multiwifi_inventory_finalize(GObject *object)
{
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(object);
    g_mutex_clear(&self->mutex);
    G_OBJECT_CLASS(multiwifi_inventory_parent_class)->finalize(object);
}

static void multiwifi_inventory_class_init(MultiWifiInventoryClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_inventory_dispose;
    oclass->finalize = multiwifi_inventory_finalize;

    s_signals[SIGNAL_DEVICES_CHANGED] = g_signal_new(
        "devices-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL,
        NULL,
        NULL,
        G_TYPE_NONE,
        0
    );
}

static void multiwifi_inventory_init(MultiWifiInventory *self)
{
    g_mutex_init(&self->mutex);
    self->cached_devices = g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_device_info_free);
}

MultiWifiInventory *multiwifi_inventory_new(void)
{
    return g_object_new(MULTIWIFI_TYPE_INVENTORY, NULL);
}

static MultiWifiDeviceInfo *extract_wifi_device_info(NMDeviceWifi *wifi_dev)
{
    NMDevice *dev = NM_DEVICE(wifi_dev);
    MultiWifiDeviceInfo *info = multiwifi_device_info_new();

    info->ifname = g_strdup(nm_device_get_iface(dev));
    info->ifindex = info->ifname ? (gint)if_nametoindex(info->ifname) : -1;
    info->managed = nm_device_get_managed(dev);

    const char *hw = nm_device_get_hw_address(dev);
    if (hw == NULL) {
        hw = nm_device_wifi_get_permanent_hw_address(wifi_dev);
    }
    info->hw_address = g_strdup(hw);

    NMDeviceState nm_state = nm_device_get_state(dev);
    info->state = multiwifi_device_state_from_nm(nm_state);
    g_free(info->state_str);
    info->state_str = g_strdup(multiwifi_device_state_to_string(info->state));

    /* Active Access Point info */
    NMAccessPoint *ap = nm_device_wifi_get_active_access_point(wifi_dev);
    if (ap != NULL) {
        GBytes *ssid_bytes = nm_access_point_get_ssid(ap);
        if (ssid_bytes != NULL) {
            gsize len = 0;
            const guint8 *data = g_bytes_get_data(ssid_bytes, &len);
            info->ssid = nm_utils_ssid_to_utf8(data, len);
        }
        info->bssid = g_strdup(nm_access_point_get_bssid(ap));
        info->signal_strength = nm_access_point_get_strength(ap);
        info->frequency = nm_access_point_get_frequency(ap);
    }

    /* IPv4 Configuration */
    NMIPConfig *ip4 = nm_device_get_ip4_config(dev);
    if (ip4 != NULL) {
        const char *gw = nm_ip_config_get_gateway(ip4);
        if (gw != NULL && *gw != '\0') {
            info->ipv4_gateway = g_strdup(gw);
        }

        GPtrArray *addresses = nm_ip_config_get_addresses(ip4);
        if (addresses != NULL && addresses->len > 0) {
            NMIPAddress *addr = g_ptr_array_index(addresses, 0);
            info->ipv4_address = g_strdup_printf(
                "%s/%u",
                nm_ip_address_get_address(addr),
                nm_ip_address_get_prefix(addr)
            );
        }
    }

    /* IPv6 Configuration */
    NMIPConfig *ip6 = nm_device_get_ip6_config(dev);
    if (ip6 != NULL) {
        GPtrArray *addresses6 = nm_ip_config_get_addresses(ip6);
        if (addresses6 != NULL && addresses6->len > 0) {
            NMIPAddress *addr6 = g_ptr_array_index(addresses6, 0);
            info->ipv6_address = g_strdup_printf(
                "%s/%u",
                nm_ip_address_get_address(addr6),
                nm_ip_address_get_prefix(addr6)
            );
        }
    }

    /* Active Connection UUID and Profile ID */
    NMActiveConnection *ac = nm_device_get_active_connection(dev);
    if (ac != NULL) {
        info->connection_uuid = g_strdup(nm_active_connection_get_uuid(ac));
        info->connection_id = g_strdup(nm_active_connection_get_id(ac));
    }

    return info;
}

static void on_device_state_changed(NMDevice *device,
                                    NMDeviceState new_state,
                                    NMDeviceState old_state,
                                    NMDeviceStateReason reason,
                                    gpointer user_data)
{
    (void)device;
    (void)new_state;
    (void)old_state;
    (void)reason;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);

    g_mutex_lock(&self->mutex);
    multiwifi_inventory_refresh_locked(self);
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
}

static void on_device_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);

    g_mutex_lock(&self->mutex);
    multiwifi_inventory_refresh_locked(self);
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
}

static void watch_device_signals(MultiWifiInventory *self, NMDevice *dev)
{
    g_signal_connect_object(dev, "state-changed",
                            G_CALLBACK(on_device_state_changed), self, 0);
    g_signal_connect_object(dev, "notify::active-access-point",
                            G_CALLBACK(on_device_notify), self, 0);
    g_signal_connect_object(dev, "notify::ip4-config",
                            G_CALLBACK(on_device_notify), self, 0);
    g_signal_connect_object(dev, "notify::ip6-config",
                            G_CALLBACK(on_device_notify), self, 0);
}

static void multiwifi_inventory_refresh_locked(MultiWifiInventory *self)
{
    g_ptr_array_set_size(self->cached_devices, 0);

    if (self->client == NULL || !nm_client_get_nm_running(self->client)) {
        return;
    }

    const GPtrArray *devices = nm_client_get_devices(self->client);
    if (devices == NULL) {
        return;
    }

    for (guint i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (NM_IS_DEVICE_WIFI(dev)) {
            MultiWifiDeviceInfo *info = extract_wifi_device_info(NM_DEVICE_WIFI(dev));
            g_ptr_array_add(self->cached_devices, info);
        }
    }
}

static void on_device_added(NMClient *client, NMDevice *device, gpointer user_data)
{
    (void)client;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);

    if (NM_IS_DEVICE_WIFI(device)) {
        g_info("Wi-Fi device added: %s", nm_device_get_iface(device));
        watch_device_signals(self, device);

        g_mutex_lock(&self->mutex);
        multiwifi_inventory_refresh_locked(self);
        g_mutex_unlock(&self->mutex);

        g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
    }
}

static void on_device_removed(NMClient *client, NMDevice *device, gpointer user_data)
{
    (void)client;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);

    if (NM_IS_DEVICE_WIFI(device)) {
        g_info("Wi-Fi device removed: %s", nm_device_get_iface(device));

        g_mutex_lock(&self->mutex);
        multiwifi_inventory_refresh_locked(self);
        g_mutex_unlock(&self->mutex);

        g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
    }
}

static void on_notify_running(NMClient *client, GParamSpec *pspec, gpointer user_data)
{
    (void)client;
    (void)pspec;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);

    g_info("NetworkManager running state changed: %s",
           nm_client_get_nm_running(self->client) ? "Running" : "Stopped");

    g_mutex_lock(&self->mutex);
    multiwifi_inventory_refresh_locked(self);
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
}

static void on_nm_client_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(user_data);
    GError *error = NULL;

    self->client = nm_client_new_finish(res, &error);
    if (error != NULL) {
        g_warning("Failed to initialize libnm client: %s", error->message);
        g_error_free(error);
        return;
    }

    g_info("libnm client initialized successfully (NM daemon running: %s)",
           nm_client_get_nm_running(self->client) ? "yes" : "no");

    g_signal_connect_object(self->client, "device-added",
                            G_CALLBACK(on_device_added), self, 0);
    g_signal_connect_object(self->client, "device-removed",
                            G_CALLBACK(on_device_removed), self, 0);
    g_signal_connect_object(self->client, "notify::nm-running",
                            G_CALLBACK(on_notify_running), self, 0);

    const GPtrArray *devices = nm_client_get_devices(self->client);
    if (devices != NULL) {
        for (guint i = 0; i < devices->len; i++) {
            NMDevice *dev = g_ptr_array_index(devices, i);
            if (NM_IS_DEVICE_WIFI(dev)) {
                watch_device_signals(self, dev);
            }
        }
    }

    g_mutex_lock(&self->mutex);
    multiwifi_inventory_refresh_locked(self);
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_DEVICES_CHANGED], 0);
}

gboolean multiwifi_inventory_start(MultiWifiInventory *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Inventory is NULL");
        return FALSE;
    }

    if (self->client != NULL) {
        return TRUE;
    }

    nm_client_new_async(NULL, on_nm_client_ready, self);
    return TRUE;
}

gboolean multiwifi_inventory_is_running(MultiWifiInventory *self)
{
    if (self == NULL || self->client == NULL) {
        return FALSE;
    }
    return nm_client_get_nm_running(self->client);
}

GPtrArray *multiwifi_inventory_dup_devices(MultiWifiInventory *self)
{
    if (self == NULL) {
        return NULL;
    }

    g_mutex_lock(&self->mutex);
    GPtrArray *dup = g_ptr_array_new_full(
        self->cached_devices->len,
        (GDestroyNotify)multiwifi_device_info_free
    );

    for (guint i = 0; i < self->cached_devices->len; i++) {
        MultiWifiDeviceInfo *src = g_ptr_array_index(self->cached_devices, i);
        g_ptr_array_add(dup, multiwifi_device_info_copy(src));
    }
    g_mutex_unlock(&self->mutex);

    return dup;
}

MultiWifiDeviceInfo *multiwifi_inventory_find_device(MultiWifiInventory *self, const gchar *ifname)
{
    if (self == NULL || ifname == NULL) {
        return NULL;
    }

    MultiWifiDeviceInfo *match = NULL;
    g_mutex_lock(&self->mutex);
    for (guint i = 0; i < self->cached_devices->len; i++) {
        MultiWifiDeviceInfo *cur = g_ptr_array_index(self->cached_devices, i);
        if (g_strcmp0(cur->ifname, ifname) == 0) {
            match = multiwifi_device_info_copy(cur);
            break;
        }
    }
    g_mutex_unlock(&self->mutex);

    return match;
}
