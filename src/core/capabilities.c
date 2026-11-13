#include "multiwifi/core/capabilities.h"
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#ifndef IPPROTO_MPTCP
#define IPPROTO_MPTCP 262
#endif

MultiWifiCapabilities *multiwifi_capabilities_new(void)
{
    MultiWifiCapabilities *caps = g_new0(MultiWifiCapabilities, 1);
    caps->kernel_mptcp = MULTIWIFI_CAP_STATE_UNKNOWN;
    caps->nftables = MULTIWIFI_CAP_STATE_UNKNOWN;
    caps->tun_device = MULTIWIFI_CAP_STATE_UNKNOWN;
    caps->network_manager = MULTIWIFI_CAP_STATE_UNKNOWN;
    caps->mptcp_details = NULL;
    caps->kernel_version = NULL;
    return caps;
}

const gchar *multiwifi_cap_state_to_string(MultiWifiCapState state)
{
    switch (state) {
    case MULTIWIFI_CAP_STATE_UNSUPPORTED:
        return "Unsupported";
    case MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE:
        return "Available (Inactive)";
    case MULTIWIFI_CAP_STATE_ACTIVE:
        return "Active";
    case MULTIWIFI_CAP_STATE_UNKNOWN:
    default:
        return "Unknown";
    }
}

void multiwifi_capabilities_probe(MultiWifiCapabilities *caps)
{
    if (caps == NULL) {
        return;
    }

    /* Record Kernel Version */
    struct utsname u;
    if (uname(&u) == 0) {
        g_free(caps->kernel_version);
        caps->kernel_version = g_strdup_printf("%s %s", u.sysname, u.release);
    } else {
        caps->kernel_version = g_strdup("Unknown Kernel");
    }

    /* Probe TUN device */
    if (access("/dev/net/tun", F_OK) == 0) {
        caps->tun_device = MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE;
    } else {
        caps->tun_device = MULTIWIFI_CAP_STATE_UNSUPPORTED;
    }

    /* Probe nftables availability */
    if (access("/usr/bin/nft", X_OK) == 0 || g_file_test("/proc/sys/net/netfilter", G_FILE_TEST_IS_DIR)) {
        caps->nftables = MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE;
    } else {
        caps->nftables = MULTIWIFI_CAP_STATE_UNSUPPORTED;
    }

    /* Probe NetworkManager running state */
    if (g_file_test("/run/NetworkManager/NetworkManager.pid", G_FILE_TEST_EXISTS) ||
        g_file_test("/var/run/NetworkManager/NetworkManager.pid", G_FILE_TEST_EXISTS)) {
        caps->network_manager = MULTIWIFI_CAP_STATE_ACTIVE;
    } else {
        caps->network_manager = MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE;
    }

    /* Probe Kernel MPTCP */
    gchar *mptcp_sysctl = NULL;
    gsize length = 0;
    gboolean sysctl_found = g_file_get_contents("/proc/sys/net/mptcp/enabled", &mptcp_sysctl, &length, NULL);

    int test_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP);
    if (test_sock >= 0) {
        close(test_sock);
        caps->kernel_mptcp = MULTIWIFI_CAP_STATE_AVAILABLE_INACTIVE;
        g_free(caps->mptcp_details);
        caps->mptcp_details = g_strdup_printf("Kernel MPTCP socket supported (sysctl enabled=%s)",
                                              sysctl_found ? g_strstrip(mptcp_sysctl) : "unknown");
    } else {
        int err = errno;
        caps->kernel_mptcp = MULTIWIFI_CAP_STATE_UNSUPPORTED;
        g_free(caps->mptcp_details);
        caps->mptcp_details = g_strdup_printf("IPPROTO_MPTCP socket failed: %s (%d)", g_strerror(err), err);
    }

    g_free(mptcp_sysctl);
}

void multiwifi_capabilities_free(MultiWifiCapabilities *caps)
{
    if (caps == NULL) {
        return;
    }
    g_free(caps->mptcp_details);
    g_free(caps->kernel_version);
    g_free(caps);
}
