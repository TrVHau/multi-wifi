#include <glib.h>
#include <glib-unix.h>
#include <NetworkManager.h>
#include "multiwifi/core/logging.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_device.h"
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

    /* Discover Wi-Fi devices via libnm */
    GError *error = NULL;
    NMClient *client = nm_client_new(NULL, &error);
    if (client != NULL) {
        const GPtrArray *devices = nm_client_get_devices(client);
        guint wifi_count = 0;
        if (devices != NULL) {
            for (guint i = 0; i < devices->len; i++) {
                NMDevice *dev = g_ptr_array_index(devices, i);
                if (NM_IS_DEVICE_WIFI(dev)) {
                    wifi_count++;
                    NMDeviceState nm_state = nm_device_get_state(dev);
                    MultiWifiDeviceState st = multiwifi_device_state_from_nm(nm_state);
                    g_info("Wi-Fi adapter #%u: %s [%s] (State: %s)",
                           wifi_count,
                           nm_device_get_iface(dev),
                           nm_device_get_hw_address(dev) ? nm_device_get_hw_address(dev) : "no-mac",
                           multiwifi_device_state_to_string(st));
                }
            }
        }
        g_info("Inventory check: %u Wi-Fi adapters detected.", wifi_count);
        g_object_unref(client);
    } else {
        g_warning("Could not connect to NetworkManager: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    g_info("Phase 1 Wi-Fi inventory verified. Exiting daemon stub cleanly.");
    return 0;
}
