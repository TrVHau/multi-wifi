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
    GCancellable *cancellable;
    gboolean starting;
    GPtrArray *cached_devices; /* MultiWifiDeviceInfo* */
    GMutex mutex;
};

G_DEFINE_TYPE(MultiWifiInventory, multiwifi_inventory, G_TYPE_OBJECT)

static void multiwifi_inventory_refresh_locked(MultiWifiInventory *self);

static void multiwifi_inventory_dispose(GObject *object)
{
    MultiWifiInventory *self = MULTIWIFI_INVENTORY(object);

    if (self->cancellable != NULL) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }

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
    self->cancellable = g_cancellable_new();
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

    /* Authoritative kernel fallback if libnm has not yet populated IPv4 address or Gateway */
    if (info->ifname != NULL && (info->ipv4_address == NULL || info->ipv4_gateway == NULL)) {
        multiwifi_wifi_device_fill_kernel_ipv4(info->ifname, &info->ipv4_address, &info->ipv4_gateway);
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
    g_signal_connect_object(dev, "notify::active-connection",
                            G_CALLBACK(on_device_notify), self, 0);
    g_signal_connect_object(dev, "notify::dhcp4-config",
                            G_CALLBACK(on_device_notify), self, 0);
    g_signal_connect_object(dev, "notify::state",
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
    NMClient *client = nm_client_new_finish(res, &error);

    self->starting = FALSE;
    if (error != NULL) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("Failed to initialize libnm client: %s", error->message);
        }
        g_error_free(error);
        g_object_unref(self);
        return;
    }

    if (self->cancellable == NULL || g_cancellable_is_cancelled(self->cancellable)) {
        g_clear_object(&client);
        g_object_unref(self);
        return;
    }

    g_clear_object(&self->client);
    self->client = client;

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
    g_object_unref(self);
}

gboolean multiwifi_inventory_start(MultiWifiInventory *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Inventory is NULL");
        return FALSE;
    }

    if (self->client != NULL || self->starting) {
        return TRUE;
    }

    if (self->cancellable == NULL) {
        self->cancellable = g_cancellable_new();
    }

    self->starting = TRUE;
    nm_client_new_async(self->cancellable, on_nm_client_ready, g_object_ref(self));
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

void multiwifi_inventory_refresh(MultiWifiInventory *self)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    multiwifi_inventory_refresh_locked(self);
    g_mutex_unlock(&self->mutex);
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

static NMDeviceWifi *find_nm_device_wifi(MultiWifiInventory *self, const gchar *ifname)
{
    if (self == NULL || self->client == NULL || ifname == NULL) {
        return NULL;
    }

    const GPtrArray *devs = nm_client_get_devices(self->client);
    if (devs == NULL) {
        return NULL;
    }

    for (guint i = 0; i < devs->len; i++) {
        NMDevice *dev = g_ptr_array_index(devs, i);
        if (NM_IS_DEVICE_WIFI(dev)) {
            const char *dev_iface = nm_device_get_iface(dev);
            if (dev_iface != NULL && g_strcmp0(dev_iface, ifname) == 0) {
                return NM_DEVICE_WIFI(dev);
            }
        }
    }
    return NULL;
}

static gchar *determine_ap_security(NMAccessPoint *ap)
{
    NM80211ApFlags flags = nm_access_point_get_flags(ap);
    NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
    NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);

    if (rsn & NM_802_11_AP_SEC_KEY_MGMT_SAE) {
        return g_strdup("WPA3");
    }
    if ((rsn & NM_802_11_AP_SEC_KEY_MGMT_PSK) && (wpa & NM_802_11_AP_SEC_KEY_MGMT_PSK)) {
        return g_strdup("WPA/WPA2");
    }
    if (rsn & NM_802_11_AP_SEC_KEY_MGMT_PSK) {
        return g_strdup("WPA2");
    }
    if (wpa & NM_802_11_AP_SEC_KEY_MGMT_PSK) {
        return g_strdup("WPA");
    }
    if (flags & NM_802_11_AP_FLAGS_PRIVACY) {
        return g_strdup("WEP");
    }
    return g_strdup("Open");
}

static NMConnection *find_saved_connection_for_ssid(MultiWifiInventory *self, const gchar *target_ssid)
{
    if (self == NULL || self->client == NULL || target_ssid == NULL) {
        return NULL;
    }

    const GPtrArray *conns = nm_client_get_connections(self->client);
    if (conns == NULL) {
        return NULL;
    }

    for (guint i = 0; i < conns->len; i++) {
        NMConnection *c = g_ptr_array_index(conns, i);
        NMSettingWireless *s_w = nm_connection_get_setting_wireless(c);
        if (s_w != NULL) {
            GBytes *s_bytes = nm_setting_wireless_get_ssid(s_w);
            if (s_bytes != NULL) {
                gsize len = 0;
                const guint8 *d = g_bytes_get_data(s_bytes, &len);
                gchar *saved_ssid = nm_utils_ssid_to_utf8(d, len);
                if (saved_ssid != NULL && g_strcmp0(saved_ssid, target_ssid) == 0) {
                    g_free(saved_ssid);
                    return c;
                }
                g_free(saved_ssid);
            }
        }
    }
    return NULL;
}

static gint compare_ap_strength(gconstpointer a, gconstpointer b)
{
    const MultiWifiAccessPointInfo *ap_a = *(const MultiWifiAccessPointInfo **)a;
    const MultiWifiAccessPointInfo *ap_b = *(const MultiWifiAccessPointInfo **)b;

    /* Active connected AP first */
    if (ap_a->is_active != ap_b->is_active) {
        return ap_a->is_active ? -1 : 1;
    }
    /* Saved connection second */
    if (ap_a->is_saved != ap_b->is_saved) {
        return ap_a->is_saved ? -1 : 1;
    }
    /* Higher signal strength first */
    return (gint)ap_b->signal_strength - (gint)ap_a->signal_strength;
}

GPtrArray *multiwifi_inventory_dup_access_points(MultiWifiInventory *self, const gchar *ifname)
{
    if (self == NULL || ifname == NULL) {
        return g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_access_point_info_free);
    }

    NMDeviceWifi *wifi_dev = find_nm_device_wifi(self, ifname);
    if (wifi_dev == NULL) {
        return g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_access_point_info_free);
    }

    NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(wifi_dev);
    const char *active_bssid = active_ap ? nm_access_point_get_bssid(active_ap) : NULL;

    const GPtrArray *raw_aps = nm_device_wifi_get_access_points(wifi_dev);
    GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_access_point_info_free);
    if (raw_aps == NULL) {
        return result;
    }

    GHashTable *seen_ssids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < raw_aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(raw_aps, i);
        GBytes *ssid_bytes = nm_access_point_get_ssid(ap);
        if (ssid_bytes == NULL) {
            continue;
        }

        gsize len = 0;
        const guint8 *data = g_bytes_get_data(ssid_bytes, &len);
        gchar *ssid_utf8 = nm_utils_ssid_to_utf8(data, len);
        if (ssid_utf8 == NULL || *ssid_utf8 == '\0') {
            g_free(ssid_utf8);
            continue;
        }

        guint8 strength = nm_access_point_get_strength(ap);
        const char *cur_bssid = nm_access_point_get_bssid(ap);
        gboolean is_active = (active_bssid != NULL && cur_bssid != NULL && g_strcmp0(active_bssid, cur_bssid) == 0);

        /* Deduplicate by SSID: Keep active or strongest */
        gpointer existing_idx = g_hash_table_lookup(seen_ssids, ssid_utf8);
        if (existing_idx != NULL) {
            guint idx = GPOINTER_TO_UINT(existing_idx) - 1;
            MultiWifiAccessPointInfo *prev = g_ptr_array_index(result, idx);
            if (!prev->is_active && (is_active || strength > prev->signal_strength)) {
                prev->signal_strength = strength;
                prev->frequency = nm_access_point_get_frequency(ap);
                g_free(prev->bssid);
                prev->bssid = g_strdup(cur_bssid);
                if (is_active) prev->is_active = TRUE;
            }
            g_free(ssid_utf8);
            continue;
        }

        MultiWifiAccessPointInfo *info = multiwifi_access_point_info_new();
        info->ssid = g_strdup(ssid_utf8);
        info->bssid = g_strdup(cur_bssid);
        info->signal_strength = strength;
        info->frequency = nm_access_point_get_frequency(ap);
        info->is_active = is_active;
        info->security_str = determine_ap_security(ap);

        NMConnection *saved_c = find_saved_connection_for_ssid(self, ssid_utf8);
        if (saved_c != NULL) {
            info->is_saved = TRUE;
            info->saved_connection_id = g_strdup(nm_connection_get_id(saved_c));
        }

        g_ptr_array_add(result, info);
        g_hash_table_insert(seen_ssids, ssid_utf8, GUINT_TO_POINTER(result->len));
    }

    g_hash_table_destroy(seen_ssids);
    g_ptr_array_sort(result, compare_ap_strength);
    return result;
}

static void on_scan_async_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    gboolean success = nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), res, &error);
    if (!success) {
        if (error == NULL) {
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Wi-Fi scan request failed");
        }
        g_task_return_error(task, error);
    } else {
        g_clear_error(&error);
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(task);
}

void multiwifi_inventory_request_scan_async(MultiWifiInventory *self,
                                            const gchar *ifname,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    NMDeviceWifi *wifi_dev = find_nm_device_wifi(self, ifname);
    if (wifi_dev == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "Wi-Fi device '%s' not found", ifname ? ifname : "unknown");
        g_object_unref(task);
        return;
    }

    nm_device_wifi_request_scan_async(wifi_dev, cancellable, on_scan_async_ready, task);
}

gboolean multiwifi_inventory_request_scan_finish(MultiWifiInventory *self,
                                                 GAsyncResult *res,
                                                 GError **error)
{
    (void)self;
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void on_activate_conn_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    NMActiveConnection *act = nm_client_activate_connection_finish(NM_CLIENT(source), res, &error);
    if (act == NULL) {
        if (error == NULL) {
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to activate Wi-Fi connection");
        }
        g_task_return_error(task, error);
    } else {
        g_object_unref(act);
        g_clear_error(&error);
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(task);
}

static void on_add_activate_conn_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    NMActiveConnection *act = nm_client_add_and_activate_connection_finish(NM_CLIENT(source), res, &error);
    if (act == NULL) {
        if (error == NULL) {
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create and activate Wi-Fi connection");
        }
        g_task_return_error(task, error);
    } else {
        g_object_unref(act);
        g_clear_error(&error);
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(task);
}

void multiwifi_inventory_connect_wifi_async(MultiWifiInventory *self,
                                            const gchar *ifname,
                                            const gchar *ssid,
                                            const gchar *password,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    if (self == NULL || self->client == NULL ||
        ifname == NULL || *ifname == '\0' ||
        ssid == NULL || *ssid == '\0') {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Invalid inventory state, adapter, or SSID for Wi-Fi connection");
        g_object_unref(task);
        return;
    }

    NMDeviceWifi *wifi_dev = find_nm_device_wifi(self, ifname);
    if (wifi_dev == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "Wi-Fi device '%s' not found", ifname);
        g_object_unref(task);
        return;
    }

    NMConnection *saved = find_saved_connection_for_ssid(self, ssid);
    if (saved != NULL) {
        NMSettingConnection *s_con = nm_connection_get_setting_connection(saved);
        const char *bound_iface = s_con ? nm_setting_connection_get_interface_name(s_con) : NULL;

        if (bound_iface == NULL || g_strcmp0(bound_iface, ifname) == 0) {
            /* Connection profile is compatible with this adapter */
            nm_client_activate_connection_async(
                self->client,
                saved,
                NM_DEVICE(wifi_dev),
                NULL,
                cancellable,
                on_activate_conn_ready,
                task
            );
            return;
        }

        /* Profile is bound to another adapter: clone connection targeting ifname */
        NMConnection *cloned = nm_simple_connection_new_clone(saved);
        NMSettingConnection *s_c = nm_connection_get_setting_connection(cloned);
        if (s_c != NULL) {
            gchar *new_uuid = g_uuid_string_random();
            gchar *new_id = g_strdup_printf("%s-%s", ssid, ifname);
            g_object_set(s_c,
                NM_SETTING_CONNECTION_UUID, new_uuid,
                NM_SETTING_CONNECTION_INTERFACE_NAME, ifname,
                NM_SETTING_CONNECTION_ID, new_id,
                NULL);
            g_free(new_uuid);
            g_free(new_id);
        }

        nm_client_add_and_activate_connection_async(
            self->client,
            cloned,
            NM_DEVICE(wifi_dev),
            NULL,
            cancellable,
            on_add_activate_conn_ready,
            task
        );
        g_object_unref(cloned);
        return;
    }

    /* Create new connection profile */
    NMConnection *conn = nm_simple_connection_new();
    NMSettingConnection *s_con = (NMSettingConnection *)nm_setting_connection_new();
    gchar *new_uuid = g_uuid_string_random();
    gchar *new_id = g_strdup_printf("%s-%s", ssid, ifname);
    g_object_set(s_con,
        NM_SETTING_CONNECTION_UUID, new_uuid,
        NM_SETTING_CONNECTION_ID, new_id,
        NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
        NM_SETTING_CONNECTION_INTERFACE_NAME, ifname,
        NULL);
    g_free(new_uuid);
    g_free(new_id);
    nm_connection_add_setting(conn, NM_SETTING(s_con));

    NMSettingWireless *s_w = (NMSettingWireless *)nm_setting_wireless_new();
    GBytes *ssid_bytes = g_bytes_new(ssid, strlen(ssid));
    g_object_set(s_w,
        NM_SETTING_WIRELESS_SSID, ssid_bytes,
        NM_SETTING_WIRELESS_MODE, "infrastructure",
        NULL);
    g_bytes_unref(ssid_bytes);
    nm_connection_add_setting(conn, NM_SETTING(s_w));

    if (password != NULL && *password != '\0') {
        NMSettingWirelessSecurity *s_sec = (NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
        g_object_set(s_sec,
            NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
            NM_SETTING_WIRELESS_SECURITY_PSK, password,
            NULL);
        nm_connection_add_setting(conn, NM_SETTING(s_sec));
    }

    nm_client_add_and_activate_connection_async(
        self->client,
        conn,
        NM_DEVICE(wifi_dev),
        NULL,
        cancellable,
        on_add_activate_conn_ready,
        task
    );
    g_object_unref(conn);
}

gboolean multiwifi_inventory_connect_wifi_finish(MultiWifiInventory *self,
                                                 GAsyncResult *res,
                                                 GError **error)
{
    (void)self;
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);
    return g_task_propagate_boolean(G_TASK(res), error);
}
