#include <gtk/gtk.h>
#include "window.h"
#include "multiwifi/core/logging.h"
#include "multiwifi/version.h"

static void on_app_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkWidget *win = multiwifi_window_new(app);
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char *argv[])
{
    multiwifi_log_init("MultiWiFi-GUI");
    g_info("Starting MultiWiFi GUI v%s (API %u)", MULTIWIFI_VERSION_STRING, MULTIWIFI_API_VERSION);

    GtkApplication *app = gtk_application_new(
        "io.github.multiwifi.MultiWiFi",
        G_APPLICATION_DEFAULT_FLAGS
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return status;
}
