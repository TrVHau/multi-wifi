#include <gtk/gtk.h>
#include "window.h"
#include "multiwifi/core/logging.h"
#include "multiwifi/version.h"

static gboolean s_use_session_bus = FALSE;
static gboolean s_light_mode = FALSE;

static const GOptionEntry s_app_entries[] = {
    { "session", 's', 0, G_OPTION_ARG_NONE, &s_use_session_bus, "Connect to daemon on session bus (for development/testing)", NULL },
    { "light", 'l', 0, G_OPTION_ARG_NONE, &s_light_mode, "Lightweight mode using Cairo 2D renderer (reduces RAM usage by >60%)", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

static void on_app_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    if (s_use_session_bus) {
        multiwifi_window_set_default_bus_type(G_BUS_TYPE_SESSION);
    }
    GtkWidget *win = multiwifi_window_new(app);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char *argv[])
{
    /* Check for lightweight mode before GTK/GDK initializes GPU backend */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--light") == 0 || g_strcmp0(argv[i], "-l") == 0) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
            break;
        }
    }

    multiwifi_log_init("MultiWiFi-GUI");
    g_info("Starting MultiWiFi GUI v%s (API %u)", MULTIWIFI_VERSION_STRING, MULTIWIFI_API_VERSION);

    GtkApplication *app = gtk_application_new(
        "io.github.multiwifi.MultiWiFi",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_application_add_main_option_entries(G_APPLICATION(app), s_app_entries);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
