#include <glib.h>
#include <glib-unix.h>
#include "multiwifi/core/logging.h"
#include "multiwifi/daemon/service.h"
#include "multiwifi/version.h"

static gboolean on_sigterm(gpointer user_data)
{
    GMainLoop *loop = (GMainLoop *)user_data;
    g_info("Received termination signal, shutting down MultiWiFi System Daemon...");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
    multiwifi_log_init("MultiWiFi-Daemon");
    g_info("Starting MultiWiFi System Daemon v%s (API %u)", MULTIWIFI_VERSION_STRING, MULTIWIFI_API_VERSION);

    gboolean use_session_bus = FALSE;
    gboolean disable_polkit = FALSE;
    gboolean show_version = FALSE;

    GOptionEntry entries[] = {
        { "session", 's', 0, G_OPTION_ARG_NONE, &use_session_bus, "Run on session bus instead of system bus (for testing)", NULL },
        { "no-polkit", 0, 0, G_OPTION_ARG_NONE, &disable_polkit, "Disable polkit authorization enforcement (for testing)", NULL },
        { "version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "Show daemon version and exit", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

    GError *error = NULL;
    GOptionContext *context = g_option_context_new("- MultiWiFi privileged networking daemon");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Failed to parse arguments: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(context);
        return 1;
    }
    g_option_context_free(context);

    if (show_version) {
        g_print("multiwifi-daemon %s (API %u)\n", MULTIWIFI_VERSION_STRING, MULTIWIFI_API_VERSION);
        return 0;
    }

    GBusType bus_type = use_session_bus ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
    gboolean enable_polkit = !disable_polkit;

    g_info("Target D-Bus: %s, Polkit: %s",
           use_session_bus ? "SESSION BUS" : "SYSTEM BUS",
           enable_polkit ? "ENABLED" : "DISABLED");

    MultiWifiDaemonService *service = multiwifi_daemon_service_new(bus_type, enable_polkit);
    if (!multiwifi_daemon_service_start(service, &error)) {
        g_critical("Failed to start D-Bus service: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(service);
        return 1;
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_sigterm, loop);
    g_unix_signal_add(SIGTERM, on_sigterm, loop);

    g_info("Daemon main loop active. Serving D-Bus interface...");
    g_main_loop_run(loop);

    g_info("Cleaning up daemon service...");
    multiwifi_daemon_service_stop(service);
    g_object_unref(service);
    g_main_loop_unref(loop);

    g_info("MultiWiFi System Daemon stopped cleanly.");
    return 0;
}
