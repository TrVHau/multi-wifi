#include "multiwifi/core/config.h"
#include <glib.h>

static void test_config_default(void)
{
    MultiWifiConfig *cfg = multiwifi_config_new_default();
    g_assert_nonnull(cfg);
    g_assert_cmpuint(cfg->schema_version, ==, 1);
    g_assert_cmpint(cfg->mode, ==, MULTIWIFI_MODE_LOAD_BALANCE);
    g_assert_cmpuint(cfg->load_balance.health_check_interval_ms, ==, 2000);

    GError *error = NULL;
    gboolean valid = multiwifi_config_validate(cfg, &error);
    g_assert_no_error(error);
    g_assert_true(valid);

    multiwifi_config_free(cfg);
}

static void test_config_parse_valid(void)
{
    const gchar *valid_toml =
        "schema_version = 1\n"
        "mode = \"load_balance\"\n"
        "\n"
        "[load_balance]\n"
        "strategy = \"weighted_round_robin\"\n"
        "health_check_interval_ms = 2500\n"
        "failure_threshold = 4\n"
        "recovery_threshold = 2\n"
        "cooldown_ms = 3000\n"
        "\n"
        "[[uplinks]]\n"
        "device_id = \"wlp0s20f3\"\n"
        "enabled = true\n"
        "weight = 2\n"
        "\n"
        "[[uplinks]]\n"
        "device_id = \"wlp0s20f0u1\"\n"
        "enabled = true\n"
        "weight = 1\n"
        "\n"
        "[bonding]\n"
        "enabled = false\n"
        "profile_id = \"\"\n";

    GError *error = NULL;
    MultiWifiConfig *cfg = multiwifi_config_parse_string(valid_toml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(cfg);
    g_assert_cmpuint(cfg->schema_version, ==, 1);
    g_assert_cmpint(cfg->mode, ==, MULTIWIFI_MODE_LOAD_BALANCE);
    g_assert_cmpuint(cfg->load_balance.health_check_interval_ms, ==, 2500);
    g_assert_cmpuint(cfg->load_balance.failure_threshold, ==, 4);
    g_assert_cmpuint(cfg->load_balance.recovery_threshold, ==, 2);
    g_assert_cmpuint(cfg->load_balance.cooldown_ms, ==, 3000);
    g_assert_cmpuint(cfg->uplinks->len, ==, 2);

    MultiWifiUplinkConfig *up0 = g_ptr_array_index(cfg->uplinks, 0);
    g_assert_cmpstr(up0->device_id, ==, "wlp0s20f3");
    g_assert_true(up0->enabled);
    g_assert_cmpuint(up0->weight, ==, 2);

    MultiWifiUplinkConfig *up1 = g_ptr_array_index(cfg->uplinks, 1);
    g_assert_cmpstr(up1->device_id, ==, "wlp0s20f0u1");
    g_assert_true(up1->enabled);
    g_assert_cmpuint(up1->weight, ==, 1);

    /* Test roundtrip serialization */
    gchar *serialized = multiwifi_config_serialize(cfg);
    g_assert_nonnull(serialized);

    MultiWifiConfig *reparsed = multiwifi_config_parse_string(serialized, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reparsed);
    g_assert_cmpuint(reparsed->schema_version, ==, cfg->schema_version);
    g_assert_cmpint(reparsed->mode, ==, cfg->mode);
    g_assert_cmpuint(reparsed->uplinks->len, ==, cfg->uplinks->len);

    g_free(serialized);
    multiwifi_config_free(reparsed);
    multiwifi_config_free(cfg);
}

static void test_config_reject_secrets(void)
{
    const gchar *malicious_toml =
        "schema_version = 1\n"
        "mode = \"load_balance\"\n"
        "wifi_password = \"super_secret_pass\"\n";

    GError *error = NULL;
    MultiWifiConfig *cfg = multiwifi_config_parse_string(malicious_toml, &error);
    g_assert_null(cfg);
    g_assert_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SECRET_FORBIDDEN);
    g_error_free(error);
}

static void test_config_reject_bad_schema_version(void)
{
    const gchar *bad_version_toml =
        "schema_version = 99\n"
        "mode = \"load_balance\"\n";

    GError *error = NULL;
    MultiWifiConfig *cfg = multiwifi_config_parse_string(bad_version_toml, &error);
    g_assert_null(cfg);
    g_assert_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SCHEMA_VERSION);
    g_error_free(error);
}

static void test_config_reject_duplicate_uplinks(void)
{
    const gchar *dup_uplinks_toml =
        "schema_version = 1\n"
        "mode = \"load_balance\"\n"
        "[[uplinks]]\n"
        "device_id = \"wlan0\"\n"
        "weight = 1\n"
        "[[uplinks]]\n"
        "device_id = \"wlan0\"\n"
        "weight = 2\n";

    GError *error = NULL;
    MultiWifiConfig *cfg = multiwifi_config_parse_string(dup_uplinks_toml, &error);
    g_assert_null(cfg);
    g_assert_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION);
    g_error_free(error);
}

static void test_config_reject_invalid_range(void)
{
    const gchar *out_of_range_toml =
        "schema_version = 1\n"
        "mode = \"load_balance\"\n"
        "[load_balance]\n"
        "health_check_interval_ms = 10\n"; /* below 100ms minimum */

    GError *error = NULL;
    MultiWifiConfig *cfg = multiwifi_config_parse_string(out_of_range_toml, &error);
    g_assert_null(cfg);
    g_assert_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION);
    g_error_free(error);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/config/default", test_config_default);
    g_test_add_func("/config/parse_valid", test_config_parse_valid);
    g_test_add_func("/config/reject_secrets", test_config_reject_secrets);
    g_test_add_func("/config/reject_bad_schema_version", test_config_reject_bad_schema_version);
    g_test_add_func("/config/reject_duplicate_uplinks", test_config_reject_duplicate_uplinks);
    g_test_add_func("/config/reject_invalid_range", test_config_reject_invalid_range);

    return g_test_run();
}
