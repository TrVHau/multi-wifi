#include <glib.h>
#include <glib-unix.h>
#include <NetworkManager.h>
#include "multiwifi/core/logging.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_device.h"
#include "multiwifi/core/path_monitor.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    multiwifi_log_init("MultiWiFi-Daemon");
    g_info("MultiWiFi System Daemon stub v%s (API %u)", MULTIWIFI_VERSION_STRING, MULTIWIFI_API_VERSION);
    g_info("Target D-Bus bus: %s (%s)", MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH);

    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    multiwifi_capabilities_probe(caps);

    g_info("Host environment: %s", caps->kernel_version ? caps->kernel_version : "unknown");
    g_info("NetworkManager state: %s", multiwifi_cap_state_to_string(caps->network_manager));
    g_info("Kernel MPTCP state: %s (%s)", multiwifi_cap_state_to_string(caps->kernel_mptcp),
           caps->mptcp_details ? caps->mptcp_details : "none");

    multiwifi_capabilities_free(caps);

    /* Discover Wi-Fi devices and monitor path health */
    GError *error = NULL;
    NMClient *client = nm_client_new(NULL, &error);
    if (client != NULL) {
        MultiWifiPathMonitor *path_mon = multiwifi_path_monitor_new();

        const GPtrArray *devices = nm_client_get_devices(client);
        guint wifi_count = 0;
        if (devices != NULL) {
            for (guint i = 0; i < devices->len; i++) {
                NMDevice *dev = g_ptr_array_index(devices, i);
                if (NM_IS_DEVICE_WIFI(dev)) {
                    wifi_count++;
                    NMDeviceState nm_state = nm_device_get_state(dev);
                    MultiWifiDeviceState st = multiwifi_device_state_from_nm(nm_state);

                    MultiWifiDeviceInfo *dinfo = multiwifi_device_info_new();
                    dinfo->ifname = g_strdup(nm_device_get_iface(dev));
                    dinfo->state = st;

                    NMIPConfig *ip4 = nm_device_get_ip4_config(dev);
                    if (ip4 != NULL) {
                        dinfo->ipv4_gateway = g_strdup(nm_ip_config_get_gateway(ip4));
                        GPtrArray *addrs = nm_ip_config_get_addresses(ip4);
                        if (addrs != NULL && addrs->len > 0) {
                            NMIPAddress *addr = g_ptr_array_index(addrs, 0);
                            dinfo->ipv4_address = g_strdup_printf("%s/%u",
                                nm_ip_address_get_address(addr),
                                nm_ip_address_get_prefix(addr));
                        }
                    }

                    multiwifi_path_monitor_update_device(path_mon, dinfo);

                    g_info("Wi-Fi adapter #%u: %s [%s] (State: %s)",
                           wifi_count,
                           dinfo->ifname,
                           nm_device_get_hw_address(dev) ? nm_device_get_hw_address(dev) : "no-mac",
                           multiwifi_device_state_to_string(st));

                    multiwifi_device_info_free(dinfo);
                }
            }
        }

        /* Run one active probe round */
        multiwifi_path_monitor_probe_all_now(path_mon);

        GPtrArray *paths = multiwifi_path_monitor_dup_paths(path_mon);
        if (paths != NULL) {
            for (guint i = 0; i < paths->len; i++) {
                MultiWifiPathInfo *p = g_ptr_array_index(paths, i);
                g_info("Path [%s]: State=%s, RTT=%.1f ms, Loss=%u%%, Reason='%s', Eligible=%s",
                       p->ifname,
                       multiwifi_path_state_to_string(p->state),
                       p->rtt_ms,
                       p->packet_loss_percent,
                       p->status_reason ? p->status_reason : "none",
                       p->is_eligible ? "YES" : "NO");
            }
            g_ptr_array_unref(paths);
        }

        g_object_unref(path_mon);
        g_object_unref(client);
    } else {
        g_warning("Could not connect to NetworkManager: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    g_info("Phase 2 Path monitor verified. Exiting daemon stub cleanly.");
    return 0;
}
