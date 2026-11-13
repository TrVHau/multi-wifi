#ifndef MULTIWIFI_CONFIG_H
#define MULTIWIFI_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define MULTIWIFI_CONFIG_ERROR multiwifi_config_error_quark()
GQuark multiwifi_config_error_quark(void);

typedef enum {
    MULTIWIFI_CONFIG_ERROR_SYNTAX,
    MULTIWIFI_CONFIG_ERROR_SCHEMA_VERSION,
    MULTIWIFI_CONFIG_ERROR_VALIDATION,
    MULTIWIFI_CONFIG_ERROR_SECRET_FORBIDDEN,
    MULTIWIFI_CONFIG_ERROR_IO
} MultiWifiConfigError;

typedef enum {
    MULTIWIFI_MODE_NONE = 0,
    MULTIWIFI_MODE_LOAD_BALANCE,
    MULTIWIFI_MODE_BONDING
} MultiWifiMode;

typedef enum {
    MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN = 0
} MultiWifiStrategy;

typedef struct {
    gchar *device_id;
    gboolean enabled;
    guint weight;
} MultiWifiUplinkConfig;

typedef struct {
    MultiWifiStrategy strategy;
    guint health_check_interval_ms;
    guint failure_threshold;
    guint recovery_threshold;
    guint cooldown_ms;
} MultiWifiLoadBalanceConfig;

typedef struct {
    gboolean enabled;
    gchar *profile_id;
} MultiWifiBondingConfig;

typedef struct {
    guint schema_version;
    MultiWifiMode mode;
    MultiWifiLoadBalanceConfig load_balance;
    GPtrArray *uplinks; /* elements of type MultiWifiUplinkConfig* */
    MultiWifiBondingConfig bonding;
} MultiWifiConfig;

/**
 * multiwifi_config_new_default:
 *
 * Creates a new MultiWifiConfig initialized with safe default settings.
 *
 * Returns: (transfer full): Newly allocated MultiWifiConfig, free with
 * multiwifi_config_free().
 */
MultiWifiConfig *multiwifi_config_new_default(void);

/**
 * multiwifi_config_parse_string:
 * @text: (transfer none): Configuration string content
 * @error: (out)(optional): Return location for a GError
 *
 * Parses and validates a configuration string.
 *
 * Returns: (transfer full): Newly allocated MultiWifiConfig or %NULL on error.
 */
MultiWifiConfig *multiwifi_config_parse_string(const gchar *text, GError **error);

/**
 * multiwifi_config_parse_file:
 * @path: (transfer none): Path to configuration file
 * @error: (out)(optional): Return location for a GError
 *
 * Reads, parses, and validates configuration from file.
 *
 * Returns: (transfer full): Newly allocated MultiWifiConfig or %NULL on error.
 */
MultiWifiConfig *multiwifi_config_parse_file(const gchar *path, GError **error);

/**
 * multiwifi_config_serialize:
 * @config: (transfer none): Configuration to serialize
 *
 * Serializes configuration into a TOML string representation.
 *
 * Returns: (transfer full): Newly allocated string, free with g_free().
 */
gchar *multiwifi_config_serialize(const MultiWifiConfig *config);

/**
 * multiwifi_config_validate:
 * @config: (transfer none): Configuration to validate
 * @error: (out)(optional): Return location for a GError
 *
 * Validates logical constraints, value ranges, and forbidden secret fields.
 *
 * Returns: %TRUE if configuration is valid, %FALSE otherwise.
 */
gboolean multiwifi_config_validate(const MultiWifiConfig *config, GError **error);

/**
 * multiwifi_config_free:
 * @config: (transfer full)(nullable): Configuration to free
 */
void multiwifi_config_free(MultiWifiConfig *config);

G_END_DECLS

#endif /* MULTIWIFI_CONFIG_H */
