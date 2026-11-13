#include <glib.h>
#include <glib-unix.h>
#include "multiwifi/core/logging.h"
#include "multiwifi/core/capabilities.h"
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

    g_info("Phase 0 foundation verified. Exiting daemon stub cleanly.");
    return 0;
}
