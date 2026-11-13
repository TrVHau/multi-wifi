#define G_LOG_USE_STRUCTURED 1

#include "multiwifi/core/logging.h"
#include <stdio.h>
#include <string.h>

static gboolean s_logging_initialized = FALSE;
static GRegex *s_secret_regex = NULL;

gchar *multiwifi_redact_secrets(const gchar *message)
{
    if (message == NULL) {
        return NULL;
    }

    if (s_secret_regex == NULL) {
        GError *error = NULL;
        /* Case-insensitive pattern to capture sensitive key-value pairs or tokens */
        s_secret_regex = g_regex_new(
            "(?i)(password|token|secret|private[_-]?key|psk|passphrase)\\s*[:=]\\s*([\"'][^\"']+[\"']|[^\\s,]+)",
            G_REGEX_OPTIMIZE,
            0,
            &error
        );
        if (error != NULL) {
            g_printerr("Failed to compile secret redaction regex: %s\n", error->message);
            g_error_free(error);
            return g_strdup(message);
        }
    }

    /* Replace secret values with [REDACTED] while keeping the key label */
    return g_regex_replace(
        s_secret_regex,
        message,
        -1,
        0,
        "\\1=[REDACTED]",
        0,
        NULL
    );
}

static GLogWriterOutput multiwifi_log_writer(
    GLogLevelFlags log_level,
    const GLogField *fields,
    gsize n_fields,
    gpointer user_data)
{
    const gchar *domain = "MultiWiFi";
    const gchar *message = "";
    const gchar *level_str = "INFO";

    (void)user_data;

    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0) {
            domain = (const gchar *)fields[i].value;
        } else if (g_strcmp0(fields[i].key, "MESSAGE") == 0) {
            message = (const gchar *)fields[i].value;
        }
    }

    switch (log_level & G_LOG_LEVEL_MASK) {
    case G_LOG_LEVEL_ERROR:
        level_str = "ERROR";
        break;
    case G_LOG_LEVEL_CRITICAL:
        level_str = "CRITICAL";
        break;
    case G_LOG_LEVEL_WARNING:
        level_str = "WARNING";
        break;
    case G_LOG_LEVEL_MESSAGE:
        level_str = "NOTICE";
        break;
    case G_LOG_LEVEL_INFO:
        level_str = "INFO";
        break;
    case G_LOG_LEVEL_DEBUG:
        level_str = "DEBUG";
        break;
    default:
        level_str = "LOG";
        break;
    }

    gchar *safe_message = multiwifi_redact_secrets(message);
    gint64 now_us = g_get_real_time();
    gint64 now_sec = now_us / G_USEC_PER_SEC;
    gint now_msec = (gint)((now_us % G_USEC_PER_SEC) / 1000);

    GDateTime *dt = g_date_time_new_from_unix_local(now_sec);
    gchar *dt_str = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M:%S") : g_strdup("unknown");

    fprintf(stderr, "[%s.%03d] [%s] [%s] %s\n",
            dt_str ? dt_str : "",
            now_msec,
            domain,
            level_str,
            safe_message ? safe_message : "");

    g_free(dt_str);
    if (dt != NULL) {
        g_date_time_unref(dt);
    }
    g_free(safe_message);

    return G_LOG_WRITER_HANDLED;
}

void multiwifi_log_init(const gchar *domain)
{
    (void)domain;
    if (s_logging_initialized) {
        return;
    }

    g_log_set_writer_func(multiwifi_log_writer, NULL, NULL);
    s_logging_initialized = TRUE;
}
