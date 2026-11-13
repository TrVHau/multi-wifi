#ifndef MULTIWIFI_WINDOW_H
#define MULTIWIFI_WINDOW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MULTIWIFI_TYPE_WINDOW (multiwifi_window_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiWindow, multiwifi_window, MULTIWIFI, WINDOW, GtkApplicationWindow)

/**
 * multiwifi_window_new:
 * @app: (transfer none): Parent GtkApplication
 *
 * Creates a new MultiWiFi application window.
 *
 * Returns: (transfer full): A new MultiWifiWindow widget
 */
GtkWidget *multiwifi_window_new(GtkApplication *app);

G_END_DECLS

#endif /* MULTIWIFI_WINDOW_H */
