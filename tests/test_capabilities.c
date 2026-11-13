#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/logging.h"
#include <glib.h>

static void test_capabilities_probe_safety(void)
{
    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    g_assert_nonnull(caps);
    g_assert_cmpint(caps->kernel_mptcp, ==, MULTIWIFI_CAP_STATE_UNKNOWN);

    /* Run probe; must run safely unprivileged without crashing */
    multiwifi_capabilities_probe(caps);

    g_assert_nonnull(caps->kernel_version);
    g_assert_nonnull(caps->mptcp_details);

    /* Verify states are valid enum values */
    g_assert_true(caps->kernel_mptcp >= MULTIWIFI_CAP_STATE_UNSUPPORTED &&
                  caps->kernel_mptcp <= MULTIWIFI_CAP_STATE_UNKNOWN);
    g_assert_true(caps->nftables >= MULTIWIFI_CAP_STATE_UNSUPPORTED &&
                  caps->nftables <= MULTIWIFI_CAP_STATE_UNKNOWN);
    g_assert_true(caps->tun_device >= MULTIWIFI_CAP_STATE_UNSUPPORTED &&
                  caps->tun_device <= MULTIWIFI_CAP_STATE_UNKNOWN);
    g_assert_true(caps->network_manager >= MULTIWIFI_CAP_STATE_UNSUPPORTED &&
                  caps->network_manager <= MULTIWIFI_CAP_STATE_UNKNOWN);

    /* Verify string conversion */
    const gchar *state_str = multiwifi_cap_state_to_string(caps->kernel_mptcp);
    g_assert_nonnull(state_str);
    g_assert_cmpuint(strlen(state_str), >, 0);

    multiwifi_capabilities_free(caps);
}

static void test_logging_secret_redaction(void)
{
    const gchar *log_with_secrets = "Connecting to SSID MyWifi with password=SuperSecretPassword123 and token=abcxyz987!";
    gchar *redacted = multiwifi_redact_secrets(log_with_secrets);

    g_assert_nonnull(redacted);
    g_assert_null(strstr(redacted, "SuperSecretPassword123"));
    g_assert_null(strstr(redacted, "abcxyz987"));
    g_assert_nonnull(strstr(redacted, "password=[REDACTED]"));
    g_assert_nonnull(strstr(redacted, "token=[REDACTED]"));

    g_free(redacted);

    /* Clean log string without secrets should remain unchanged */
    const gchar *clean_log = "Interface wlp0s20f0u1 state changed to UP (carrier detected)";
    gchar *clean_res = multiwifi_redact_secrets(clean_log);
    g_assert_cmpstr(clean_res, ==, clean_log);
    g_free(clean_res);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/capabilities/probe_safety", test_capabilities_probe_safety);
    g_test_add_func("/logging/secret_redaction", test_logging_secret_redaction);

    return g_test_run();
}
