#ifndef MULTIWIFI_WIFI_DEVICE_H
#define MULTIWIFI_WIFI_DEVICE_H

#include <glib.h>
#include <NetworkManager.h>

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_DEVICE_STATE_UNKNOWN = 0,
    MULTIWIFI_DEVICE_STATE_UNAVAILABLE,
    MULTIWIFI_DEVICE_STATE_DISCONNECTED,
    MULTIWIFI_DEVICE_STATE_PREPARING,
    MULTIWIFI_DEVICE_STATE_CONFIGURING,
    MULTIWIFI_DEVICE_STATE_NEED_AUTH,
    MULTIWIFI_DEVICE_STATE_IP_CONFIG,
    MULTIWIFI_DEVICE_STATE_CONNECTED,
    MULTIWIFI_DEVICE_STATE_FAILED
} MultiWifiDeviceState;

typedef struct {
    gchar *ifname;              /* Interface name, e.g. "wlp0s20f0u1" (owned) */
    gint ifindex;               /* Kernel interface index */
    gchar *hw_address;          /* Permanent or current MAC address (owned) */
    MultiWifiDeviceState state; /* Normalized device state */
    gchar *state_str;           /* Human-readable state string (owned) */
    gchar *ssid;                /* Active connected SSID or NULL (owned) */
    gchar *bssid;               /* Active AP BSSID or NULL (owned) */
    guint8 signal_strength;     /* 0 - 100 percent */
    guint32 frequency;          /* Frequency in MHz (e.g. 2437) */
    gchar *ipv4_address;        /* Primary IPv4 address with prefix or NULL (owned) */
    gchar *ipv4_gateway;        /* IPv4 default gateway or NULL (owned) */
    gchar *ipv6_address;        /* Primary IPv6 address with prefix or NULL (owned) */
    gchar *connection_uuid;     /* NetworkManager active connection UUID or NULL (owned) */
    gchar *connection_id;       /* Connection profile name or NULL (owned) */
    gboolean managed;           /* Whether NM manages this interface */
} MultiWifiDeviceInfo;

/**
 * multiwifi_device_info_new:
 *
 * Allocates and initializes an empty MultiWifiDeviceInfo struct.
 *
 * Returns: (transfer full): Newly allocated device info, free with
 * multiwifi_device_info_free().
 */
MultiWifiDeviceInfo *multiwifi_device_info_new(void);

/**
 * multiwifi_device_info_copy:
 * @src: (transfer none): Source struct to duplicate
 *
 * Performs a deep copy of @src.
 *
 * Returns: (transfer full): A newly allocated deep copy of @src.
 */
MultiWifiDeviceInfo *multiwifi_device_info_copy(const MultiWifiDeviceInfo *src);

/**
 * multiwifi_device_info_free:
 * @info: (transfer full)(nullable): Struct to free
 */
void multiwifi_device_info_free(MultiWifiDeviceInfo *info);

typedef struct {
    gchar *ssid;                /* UTF-8 SSID name (owned) */
    gchar *bssid;               /* BSSID MAC address (owned) */
    guint8 signal_strength;     /* 0 - 100 percent */
    guint32 frequency;          /* Frequency in MHz */
    gboolean is_saved;          /* Whether a connection profile already exists */
    gchar *saved_connection_id; /* Saved profile name if any (owned) */
    gchar *security_str;        /* e.g. "WPA2", "WPA3", "WPA/WPA2", "Open" (owned) */
    gboolean is_active;         /* Currently connected AP for this device */
} MultiWifiAccessPointInfo;

/**
 * multiwifi_access_point_info_new:
 *
 * Allocates and initializes an empty MultiWifiAccessPointInfo struct.
 *
 * Returns: (transfer full): Newly allocated AP info.
 */
MultiWifiAccessPointInfo *multiwifi_access_point_info_new(void);

/**
 * multiwifi_access_point_info_copy:
 * @src: (transfer none): Source struct to duplicate
 *
 * Returns: (transfer full): A deep copy of @src.
 */
MultiWifiAccessPointInfo *multiwifi_access_point_info_copy(const MultiWifiAccessPointInfo *src);

/**
 * multiwifi_access_point_info_free:
 * @info: (transfer full)(nullable): Struct to free
 */
void multiwifi_access_point_info_free(MultiWifiAccessPointInfo *info);

/**
 * multiwifi_device_state_from_nm:
 * @nm_state: NMDeviceState value
 *
 * Maps a NetworkManager device state to MultiWifiDeviceState.
 */
MultiWifiDeviceState multiwifi_device_state_from_nm(NMDeviceState nm_state);

/**
 * multiwifi_device_state_to_string:
 * @state: MultiWifiDeviceState
 *
 * Returns: (transfer none): Static human-readable string representation.
 */
const gchar *multiwifi_device_state_to_string(MultiWifiDeviceState state);

/**
 * multiwifi_device_info_to_string:
 * @info: (transfer none): Device info struct
 *
 * Returns: (transfer full): Formatted summary string for logging/debugging.
 */
gchar *multiwifi_device_info_to_string(const MultiWifiDeviceInfo *info);

/**
 * multiwifi_wifi_device_fill_kernel_ipv4:
 * @ifname: Interface name (e.g. "wlp0s20f3")
 * @out_addr: (inout)(transfer full)(nullable): Optional IPv4 CIDR destination (filled if NULL or empty)
 * @out_gw: (inout)(transfer full)(nullable): Optional IPv4 default gateway destination (filled if NULL or empty)
 *
 * Queries kernel interface addresses (`getifaddrs`) and routing table (`/proc/net/route`)
 * as a fallback when libnm has not yet exposed IPv4 configuration.
 */
void multiwifi_wifi_device_fill_kernel_ipv4(const gchar *ifname,
                                            gchar **out_addr,
                                            gchar **out_gw);

G_END_DECLS

#endif /* MULTIWIFI_WIFI_DEVICE_H */
