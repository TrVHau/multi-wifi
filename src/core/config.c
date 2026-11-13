#include "multiwifi/core/config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

G_DEFINE_QUARK(multiwifi-config-error-quark, multiwifi_config_error)

typedef enum {
    SECTION_ROOT,
    SECTION_LOAD_BALANCE,
    SECTION_UPLINKS,
    SECTION_BONDING
} ParserSection;

static MultiWifiUplinkConfig *uplink_config_new(const gchar *device_id, gboolean enabled, guint weight)
{
    MultiWifiUplinkConfig *up = g_new0(MultiWifiUplinkConfig, 1);
    up->device_id = g_strdup(device_id);
    up->enabled = enabled;
    up->weight = weight;
    return up;
}

static void uplink_config_free(MultiWifiUplinkConfig *up)
{
    if (up == NULL) {
        return;
    }
    g_free(up->device_id);
    g_free(up);
}

MultiWifiConfig *multiwifi_config_new_default(void)
{
    MultiWifiConfig *cfg = g_new0(MultiWifiConfig, 1);
    cfg->schema_version = 1;
    cfg->mode = MULTIWIFI_MODE_LOAD_BALANCE;

    cfg->load_balance.strategy = MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN;
    cfg->load_balance.health_check_interval_ms = 2000;
    cfg->load_balance.failure_threshold = 3;
    cfg->load_balance.recovery_threshold = 3;
    cfg->load_balance.cooldown_ms = 5000;

    cfg->uplinks = g_ptr_array_new_with_free_func((GDestroyNotify)uplink_config_free);

    cfg->bonding.enabled = FALSE;
    cfg->bonding.profile_id = NULL;

    return cfg;
}

static gchar *strip_quotes(const gchar *val)
{
    if (val == NULL) {
        return NULL;
    }
    gchar *trimmed = g_strstrip(g_strdup(val));
    gsize len = strlen(trimmed);
    if (len >= 2 && ((trimmed[0] == '"' && trimmed[len - 1] == '"') ||
                     (trimmed[0] == '\'' && trimmed[len - 1] == '\''))) {
        trimmed[len - 1] = '\0';
        gchar *unquoted = g_strdup(trimmed + 1);
        g_free(trimmed);
        return unquoted;
    }
    return trimmed;
}

static gboolean is_valid_identifier(const gchar *str)
{
    if (str == NULL || *str == '\0') {
        return FALSE;
    }
    for (const gchar *p = str; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') {
            return FALSE;
        }
    }
    return TRUE;
}

gboolean multiwifi_config_validate(const MultiWifiConfig *config, GError **error)
{
    if (config == NULL) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "Config pointer is NULL");
        return FALSE;
    }

    if (config->schema_version != 1) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SCHEMA_VERSION,
                    "Unsupported schema_version %u (supported version is 1)", config->schema_version);
        return FALSE;
    }

    if (config->mode < MULTIWIFI_MODE_NONE || config->mode > MULTIWIFI_MODE_BONDING) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "Invalid operational mode specified");
        return FALSE;
    }

    if (config->load_balance.health_check_interval_ms < 100 ||
        config->load_balance.health_check_interval_ms > 60000) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "health_check_interval_ms (%u) must be between 100 and 60000 ms",
                    config->load_balance.health_check_interval_ms);
        return FALSE;
    }

    if (config->load_balance.failure_threshold < 1 || config->load_balance.failure_threshold > 50) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "failure_threshold (%u) must be between 1 and 50",
                    config->load_balance.failure_threshold);
        return FALSE;
    }

    if (config->load_balance.recovery_threshold < 1 || config->load_balance.recovery_threshold > 50) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "recovery_threshold (%u) must be between 1 and 50",
                    config->load_balance.recovery_threshold);
        return FALSE;
    }

    if (config->load_balance.cooldown_ms > 600000) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                    "cooldown_ms (%u) exceeds maximum limit of 600000 ms",
                    config->load_balance.cooldown_ms);
        return FALSE;
    }

    if (config->uplinks != NULL) {
        GHashTable *seen_devices = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < config->uplinks->len; i++) {
            MultiWifiUplinkConfig *up = g_ptr_array_index(config->uplinks, i);
            if (!is_valid_identifier(up->device_id)) {
                g_hash_table_destroy(seen_devices);
                g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                            "Invalid or empty uplink device_id '%s'", up->device_id ? up->device_id : "");
                return FALSE;
            }
            if (g_hash_table_contains(seen_devices, up->device_id)) {
                g_hash_table_destroy(seen_devices);
                g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                            "Duplicate uplink device_id '%s' found", up->device_id);
                return FALSE;
            }
            g_hash_table_add(seen_devices, up->device_id);

            if (up->weight < 1 || up->weight > 1000) {
                g_hash_table_destroy(seen_devices);
                g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                            "Uplink '%s' weight (%u) must be between 1 and 1000", up->device_id, up->weight);
                return FALSE;
            }
        }
        g_hash_table_destroy(seen_devices);
    }

    if (config->bonding.enabled) {
        if (!is_valid_identifier(config->bonding.profile_id)) {
            g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                        "Bonding enabled but profile_id '%s' is missing or invalid",
                        config->bonding.profile_id ? config->bonding.profile_id : "");
            return FALSE;
        }
    }

    return TRUE;
}

MultiWifiConfig *multiwifi_config_parse_string(const gchar *text, GError **error)
{
    if (text == NULL) {
        g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SYNTAX, "Empty config text");
        return NULL;
    }

    MultiWifiConfig *cfg = multiwifi_config_new_default();
    gchar **lines = g_strsplit(text, "\n", -1);
    ParserSection section = SECTION_ROOT;
    MultiWifiUplinkConfig *current_uplink = NULL;

    for (gsize i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(g_strdup(lines[i]));

        /* Skip empty lines and comment lines */
        if (line[0] == '\0' || line[0] == '#') {
            g_free(line);
            continue;
        }

        /* Check for secret keywords - ordinary config must never contain secrets */
        gchar *lower_line = g_ascii_strdown(line, -1);
        if (strstr(lower_line, "password") || strstr(lower_line, "secret") ||
            strstr(lower_line, "private_key") || strstr(lower_line, "token") ||
            strstr(lower_line, "psk") || strstr(lower_line, "passphrase")) {
            g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SECRET_FORBIDDEN,
                        "Line %zu contains forbidden secret field; MultiWiFi config never stores secrets", i + 1);
            g_free(lower_line);
            g_free(line);
            g_strfreev(lines);
            multiwifi_config_free(cfg);
            return NULL;
        }
        g_free(lower_line);

        /* Section header check */
        if (g_str_has_prefix(line, "[[") && g_str_has_suffix(line, "]]")) {
            gchar *sec_name = g_strstrip(g_strndup(line + 2, strlen(line) - 4));
            if (g_strcmp0(sec_name, "uplinks") == 0) {
                section = SECTION_UPLINKS;
                current_uplink = uplink_config_new("", TRUE, 1);
                g_ptr_array_add(cfg->uplinks, current_uplink);
            } else {
                g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SYNTAX,
                            "Unknown array table '[[%s]]' at line %zu", sec_name, i + 1);
                g_free(sec_name);
                g_free(line);
                g_strfreev(lines);
                multiwifi_config_free(cfg);
                return NULL;
            }
            g_free(sec_name);
            g_free(line);
            continue;
        }

        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            gchar *sec_name = g_strstrip(g_strndup(line + 1, strlen(line) - 2));
            if (g_strcmp0(sec_name, "load_balance") == 0) {
                section = SECTION_LOAD_BALANCE;
            } else if (g_strcmp0(sec_name, "bonding") == 0) {
                section = SECTION_BONDING;
            } else {
                g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SYNTAX,
                            "Unknown section '[%s]' at line %zu", sec_name, i + 1);
                g_free(sec_name);
                g_free(line);
                g_strfreev(lines);
                multiwifi_config_free(cfg);
                return NULL;
            }
            g_free(sec_name);
            g_free(line);
            continue;
        }

        /* Key = Value pair */
        gchar **kv = g_strsplit(line, "=", 2);
        if (kv[0] == NULL || kv[1] == NULL) {
            g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_SYNTAX,
                        "Syntax error at line %zu: expected key = value", i + 1);
            g_strfreev(kv);
            g_free(line);
            g_strfreev(lines);
            multiwifi_config_free(cfg);
            return NULL;
        }

        gchar *key = g_strstrip(g_strdup(kv[0]));
        gchar *val = strip_quotes(kv[1]);

        if (section == SECTION_ROOT) {
            if (g_strcmp0(key, "schema_version") == 0) {
                cfg->schema_version = (guint)g_ascii_strtoull(val, NULL, 10);
            } else if (g_strcmp0(key, "mode") == 0) {
                if (g_strcmp0(val, "load_balance") == 0) {
                    cfg->mode = MULTIWIFI_MODE_LOAD_BALANCE;
                } else if (g_strcmp0(val, "bonding") == 0) {
                    cfg->mode = MULTIWIFI_MODE_BONDING;
                } else if (g_strcmp0(val, "none") == 0) {
                    cfg->mode = MULTIWIFI_MODE_NONE;
                } else {
                    g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                                "Unknown mode '%s' at line %zu", val, i + 1);
                    g_free(key);
                    g_free(val);
                    g_strfreev(kv);
                    g_free(line);
                    g_strfreev(lines);
                    multiwifi_config_free(cfg);
                    return NULL;
                }
            }
        } else if (section == SECTION_LOAD_BALANCE) {
            if (g_strcmp0(key, "strategy") == 0) {
                if (g_strcmp0(val, "weighted_round_robin") == 0) {
                    cfg->load_balance.strategy = MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN;
                } else {
                    g_set_error(error, MULTIWIFI_CONFIG_ERROR, MULTIWIFI_CONFIG_ERROR_VALIDATION,
                                "Unknown load balance strategy '%s' at line %zu", val, i + 1);
                    g_free(key);
                    g_free(val);
                    g_strfreev(kv);
                    g_free(line);
                    g_strfreev(lines);
                    multiwifi_config_free(cfg);
                    return NULL;
                }
            } else if (g_strcmp0(key, "health_check_interval_ms") == 0) {
                cfg->load_balance.health_check_interval_ms = (guint)g_ascii_strtoull(val, NULL, 10);
            } else if (g_strcmp0(key, "failure_threshold") == 0) {
                cfg->load_balance.failure_threshold = (guint)g_ascii_strtoull(val, NULL, 10);
            } else if (g_strcmp0(key, "recovery_threshold") == 0) {
                cfg->load_balance.recovery_threshold = (guint)g_ascii_strtoull(val, NULL, 10);
            } else if (g_strcmp0(key, "cooldown_ms") == 0) {
                cfg->load_balance.cooldown_ms = (guint)g_ascii_strtoull(val, NULL, 10);
            }
        } else if (section == SECTION_UPLINKS) {
            if (current_uplink != NULL) {
                if (g_strcmp0(key, "device_id") == 0) {
                    g_free(current_uplink->device_id);
                    current_uplink->device_id = g_strdup(val);
                } else if (g_strcmp0(key, "enabled") == 0) {
                    current_uplink->enabled = (g_ascii_strcasecmp(val, "true") == 0 || g_strcmp0(val, "1") == 0);
                } else if (g_strcmp0(key, "weight") == 0) {
                    current_uplink->weight = (guint)g_ascii_strtoull(val, NULL, 10);
                }
            }
        } else if (section == SECTION_BONDING) {
            if (g_strcmp0(key, "enabled") == 0) {
                cfg->bonding.enabled = (g_ascii_strcasecmp(val, "true") == 0 || g_strcmp0(val, "1") == 0);
            } else if (g_strcmp0(key, "profile_id") == 0) {
                g_free(cfg->bonding.profile_id);
                cfg->bonding.profile_id = g_strdup(val);
            }
        }

        g_free(key);
        g_free(val);
        g_strfreev(kv);
        g_free(line);
    }

    g_strfreev(lines);

    if (!multiwifi_config_validate(cfg, error)) {
        multiwifi_config_free(cfg);
        return NULL;
    }

    return cfg;
}

MultiWifiConfig *multiwifi_config_parse_file(const gchar *path, GError **error)
{
    gchar *content = NULL;
    gsize length = 0;
    GError *io_err = NULL;

    if (!g_file_get_contents(path, &content, &length, &io_err)) {
        g_propagate_prefixed_error(error, io_err, "Cannot read config file '%s': ", path);
        return NULL;
    }

    MultiWifiConfig *cfg = multiwifi_config_parse_string(content, error);
    g_free(content);
    return cfg;
}

gchar *multiwifi_config_serialize(const MultiWifiConfig *config)
{
    if (config == NULL) {
        return NULL;
    }

    GString *str = g_string_new(NULL);
    const gchar *mode_str = "none";
    if (config->mode == MULTIWIFI_MODE_LOAD_BALANCE) {
        mode_str = "load_balance";
    } else if (config->mode == MULTIWIFI_MODE_BONDING) {
        mode_str = "bonding";
    }

    g_string_append_printf(str, "schema_version = %u\n", config->schema_version);
    g_string_append_printf(str, "mode = \"%s\"\n\n", mode_str);

    g_string_append(str, "[load_balance]\n");
    g_string_append(str, "strategy = \"weighted_round_robin\"\n");
    g_string_append_printf(str, "health_check_interval_ms = %u\n", config->load_balance.health_check_interval_ms);
    g_string_append_printf(str, "failure_threshold = %u\n", config->load_balance.failure_threshold);
    g_string_append_printf(str, "recovery_threshold = %u\n", config->load_balance.recovery_threshold);
    g_string_append_printf(str, "cooldown_ms = %u\n\n", config->load_balance.cooldown_ms);

    if (config->uplinks != NULL) {
        for (guint i = 0; i < config->uplinks->len; i++) {
            MultiWifiUplinkConfig *up = g_ptr_array_index(config->uplinks, i);
            g_string_append(str, "[[uplinks]]\n");
            g_string_append_printf(str, "device_id = \"%s\"\n", up->device_id ? up->device_id : "");
            g_string_append_printf(str, "enabled = %s\n", up->enabled ? "true" : "false");
            g_string_append_printf(str, "weight = %u\n\n", up->weight);
        }
    }

    g_string_append(str, "[bonding]\n");
    g_string_append_printf(str, "enabled = %s\n", config->bonding.enabled ? "true" : "false");
    g_string_append_printf(str, "profile_id = \"%s\"\n", config->bonding.profile_id ? config->bonding.profile_id : "");

    return g_string_free(str, FALSE);
}

void multiwifi_config_free(MultiWifiConfig *config)
{
    if (config == NULL) {
        return;
    }
    if (config->uplinks != NULL) {
        g_ptr_array_unref(config->uplinks);
    }
    g_free(config->bonding.profile_id);
    g_free(config);
}
