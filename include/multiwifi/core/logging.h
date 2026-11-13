#ifndef MULTIWIFI_LOGGING_H
#define MULTIWIFI_LOGGING_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * multiwifi_log_init:
 * @domain: (nullable): Default log domain or %NULL for "MultiWiFi"
 *
 * Initializes GLib structured logging with MultiWiFi format and secret redaction.
 * Safe to call multiple times; idempotent.
 */
void multiwifi_log_init(const gchar *domain);

/**
 * multiwifi_redact_secrets:
 * @message: (transfer none): Input string to inspect
 *
 * Scans @message for sensitive keys and patterns (passwords, tokens, private keys)
 * and returns a newly allocated string with secrets replaced by [REDACTED].
 *
 * Returns: (transfer full): Newly allocated redacted string, free with g_free().
 */
gchar *multiwifi_redact_secrets(const gchar *message);

G_END_DECLS

#endif /* MULTIWIFI_LOGGING_H */
