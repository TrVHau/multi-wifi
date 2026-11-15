#include "multiwifi/core/wifi_device.h"
#include <stdio.h>

MultiWifiDeviceInfo *multiwifi_device_info_new(void)
{
    MultiWifiDeviceInfo *info = g_new0(MultiWifiDeviceInfo, 1);
    info->ifindex = -1;
    info->state = MULTIWIFI_DEVICE_STATE_UNKNOWN;
    info->state_str = g_strdup("Unknown");
    info->managed = TRUE;
    return info;
}

MultiWifiDeviceInfo *multiwifi_device_info_copy(const MultiWifiDeviceInfo *src)
{
    if (src == NULL) {
        return NULL;
    }

    MultiWifiDeviceInfo *dst = g_new0(MultiWifiDeviceInfo, 1);
    dst->ifname = g_strdup(src->ifname);
    dst->ifindex = src->ifindex;
    dst->hw_address = g_strdup(src->hw_address);
    dst->state = src->state;
    dst->state_str = g_strdup(src->state_str);
    dst->ssid = g_strdup(src->ssid);
    dst->bssid = g_strdup(src->bssid);
    dst->signal_strength = src->signal_strength;
    dst->frequency = src->frequency;
    dst->ipv4_address = g_strdup(src->ipv4_address);
    dst->ipv4_gateway = g_strdup(src->ipv4_gateway);
    dst->ipv6_address = g_strdup(src->ipv6_address);
    dst->connection_uuid = g_strdup(src->connection_uuid);
    dst->connection_id = g_strdup(src->connection_id);
    dst->managed = src->managed;

    return dst;
}

void multiwifi_device_info_free(MultiWifiDeviceInfo *info)
{
    if (info == NULL) {
        return;
    }

    g_free(info->ifname);
    g_free(info->hw_address);
    g_free(info->state_str);
    g_free(info->ssid);
    g_free(info->bssid);
    g_free(info->ipv4_address);
    g_free(info->ipv4_gateway);
    g_free(info->ipv6_address);
    g_free(info->connection_uuid);
    g_free(info->connection_id);
    g_free(info);
}

MultiWifiDeviceState multiwifi_device_state_from_nm(NMDeviceState nm_state)
{
    switch (nm_state) {
    case NM_DEVICE_STATE_UNMANAGED:
    case NM_DEVICE_STATE_UNAVAILABLE:
        return MULTIWIFI_DEVICE_STATE_UNAVAILABLE;
    case NM_DEVICE_STATE_DISCONNECTED:
    case NM_DEVICE_STATE_DEACTIVATING:
        return MULTIWIFI_DEVICE_STATE_DISCONNECTED;
    case NM_DEVICE_STATE_PREPARE:
        return MULTIWIFI_DEVICE_STATE_PREPARING;
    case NM_DEVICE_STATE_CONFIG:
        return MULTIWIFI_DEVICE_STATE_CONFIGURING;
    case NM_DEVICE_STATE_NEED_AUTH:
        return MULTIWIFI_DEVICE_STATE_NEED_AUTH;
    case NM_DEVICE_STATE_IP_CONFIG:
    case NM_DEVICE_STATE_IP_CHECK:
    case NM_DEVICE_STATE_SECONDARIES:
        return MULTIWIFI_DEVICE_STATE_IP_CONFIG;
    case NM_DEVICE_STATE_ACTIVATED:
        return MULTIWIFI_DEVICE_STATE_CONNECTED;
    case NM_DEVICE_STATE_FAILED:
        return MULTIWIFI_DEVICE_STATE_FAILED;
    case NM_DEVICE_STATE_UNKNOWN:
    default:
        return MULTIWIFI_DEVICE_STATE_UNKNOWN;
    }
}

const gchar *multiwifi_device_state_to_string(MultiWifiDeviceState state)
{
    switch (state) {
    case MULTIWIFI_DEVICE_STATE_CONNECTED:
        return "Connected";
    case MULTIWIFI_DEVICE_STATE_DISCONNECTED:
        return "Disconnected";
    case MULTIWIFI_DEVICE_STATE_PREPARING:
        return "Preparing";
    case MULTIWIFI_DEVICE_STATE_CONFIGURING:
        return "Configuring";
    case MULTIWIFI_DEVICE_STATE_NEED_AUTH:
        return "Needs Authentication";
    case MULTIWIFI_DEVICE_STATE_IP_CONFIG:
        return "Obtaining IP";
    case MULTIWIFI_DEVICE_STATE_FAILED:
        return "Failed";
    case MULTIWIFI_DEVICE_STATE_UNAVAILABLE:
        return "Unavailable";
    case MULTIWIFI_DEVICE_STATE_UNKNOWN:
    default:
        return "Unknown";
    }
}

gchar *multiwifi_device_info_to_string(const MultiWifiDeviceInfo *info)
{
    if (info == NULL) {
        return g_strdup("(null device)");
    }

    return g_strdup_printf(
        "Interface: %s (idx: %d, MAC: %s, State: %s)\n"
        "  SSID: %s, Signal: %u%%, Freq: %u MHz\n"
        "  IPv4: %s (Gateway: %s)\n"
        "  IPv6: %s\n"
        "  Connection: %s (UUID: %s)",
        info->ifname ? info->ifname : "unknown",
        info->ifindex,
        info->hw_address ? info->hw_address : "none",
        info->state_str ? info->state_str : "unknown",
        info->ssid ? info->ssid : "<none>",
        info->signal_strength,
        info->frequency,
        info->ipv4_address ? info->ipv4_address : "<none>",
        info->ipv4_gateway ? info->ipv4_gateway : "<none>",
        info->ipv6_address ? info->ipv6_address : "<none>",
        info->connection_id ? info->connection_id : "<none>",
        info->connection_uuid ? info->connection_uuid : "<none>"
    );
}

MultiWifiAccessPointInfo *multiwifi_access_point_info_new(void)
{
    return g_new0(MultiWifiAccessPointInfo, 1);
}

MultiWifiAccessPointInfo *multiwifi_access_point_info_copy(const MultiWifiAccessPointInfo *src)
{
    if (src == NULL) {
        return NULL;
    }

    MultiWifiAccessPointInfo *dst = g_new0(MultiWifiAccessPointInfo, 1);
    dst->ssid = g_strdup(src->ssid);
    dst->bssid = g_strdup(src->bssid);
    dst->signal_strength = src->signal_strength;
    dst->frequency = src->frequency;
    dst->is_saved = src->is_saved;
    dst->saved_connection_id = g_strdup(src->saved_connection_id);
    dst->security_str = g_strdup(src->security_str);
    dst->is_active = src->is_active;
    return dst;
}

void multiwifi_access_point_info_free(MultiWifiAccessPointInfo *info)
{
    if (info == NULL) {
        return;
    }

    g_free(info->ssid);
    g_free(info->bssid);
    g_free(info->saved_connection_id);
    g_free(info->security_str);
    g_free(info);
}
