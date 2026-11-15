#include "window.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/core/path_monitor.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

/* Embedded CSS Theme Fallback */
static const char CSS_FALLBACK_PART1[] =
"window, .main-window { background-color: #16171f; color: #cdd6f4; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Ubuntu, sans-serif; }\n"
"headerbar { background-color: #1e1e2e; border-bottom: 1px solid rgba(255, 255, 255, 0.08); color: #cdd6f4; padding: 6px 12px; }\n"
"headerbar .title { font-weight: 700; font-size: 14px; color: #cdd6f4; }\n"
".modern-card { background-color: #1e1e2e; border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 14px; padding: 16px 18px; margin-bottom: 4px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.22); }\n"
".hero-card { background: linear-gradient(145deg, #1e1e2e 0%, #26273b 100%); border: 1px solid rgba(137, 180, 250, 0.22); border-radius: 14px; padding: 16px 18px; margin-bottom: 4px; box-shadow: 0 6px 20px rgba(0, 0, 0, 0.28); }\n"
".action-row { background-color: rgba(255, 255, 255, 0.03); border: 1px solid rgba(255, 255, 255, 0.06); border-radius: 10px; padding: 10px 14px; margin-bottom: 4px; }\n"
".action-row:hover { background-color: rgba(255, 255, 255, 0.06); border-color: rgba(255, 255, 255, 0.12); }\n"
".section-header { font-size: 11px; font-weight: 800; letter-spacing: 1.2px; color: #89b4fa; text-transform: uppercase; }\n";

static const char CSS_FALLBACK_PART2[] =
".badge { border-radius: 9999px; padding: 3px 10px; font-size: 11px; font-weight: 700; }\n"
".badge-online { background-color: rgba(166, 227, 161, 0.16); color: #a6e3a1; border: 1px solid rgba(166, 227, 161, 0.35); }\n"
".badge-degraded { background-color: rgba(249, 226, 175, 0.16); color: #f9e2af; border: 1px solid rgba(249, 226, 175, 0.35); }\n"
".badge-danger { background-color: rgba(243, 139, 168, 0.16); color: #f38ba8; border: 1px solid rgba(243, 139, 168, 0.35); }\n"
".badge-active { background-color: rgba(137, 180, 250, 0.18); color: #89b4fa; border: 1px solid rgba(137, 180, 250, 0.4); }\n"
".badge-offline { background-color: rgba(108, 112, 134, 0.16); color: #a6adc8; border: 1px solid rgba(108, 112, 134, 0.28); }\n"
".badge-hw { background-color: rgba(148, 226, 213, 0.14); color: #94e2d5; border: 1px solid rgba(148, 226, 213, 0.32); }\n"
".badge-saved { background-color: rgba(203, 166, 247, 0.16); color: #cba6f7; border: 1px solid rgba(203, 166, 247, 0.35); }\n"
".metric-chip { background-color: rgba(0, 0, 0, 0.25); border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 8px; padding: 4px 10px; font-size: 11px; font-weight: 600; color: #cdd6f4; }\n"
"button.btn-primary { background: linear-gradient(135deg, #89b4fa 0%, #3584e4 100%); color: #11111b; font-weight: 700; border-radius: 8px; border: none; padding: 6px 14px; }\n"
"button.btn-primary:hover { background: linear-gradient(135deg, #b4befe 0%, #4a90e2 100%); }\n"
"button.btn-secondary { background-color: rgba(255, 255, 255, 0.06); color: #cdd6f4; border-radius: 8px; border: 1px solid rgba(255, 255, 255, 0.12); font-weight: 600; padding: 6px 14px; }\n"
"button.btn-secondary:hover { background-color: rgba(255, 255, 255, 0.12); border-color: rgba(255, 255, 255, 0.25); }\n"
"listview, list { background-color: #181825; border-radius: 10px; border: 1px solid rgba(255, 255, 255, 0.08); }\n"
"row { padding: 8px 12px; border-radius: 8px; margin: 2px 4px; }\n"
"row:selected { background-color: rgba(137, 180, 250, 0.25); color: #ffffff; }\n"
"row:hover:not(:selected) { background-color: rgba(255, 255, 255, 0.05); }\n"
"entry, passwordentry { background-color: #181825; color: #cdd6f4; border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px; padding: 8px 12px; }\n"
"entry:focus, passwordentry:focus { border-color: #89b4fa; }\n"
"switch { border-radius: 14px; padding: 2px; }\n"
"switch:checked { background-color: #89b4fa; }\n";

struct _MultiWifiWindow {
    GtkApplicationWindow parent_instance;
    GtkWidget *daemon_status_lbl;
    GtkWidget *daemon_toggle_btn;
    GtkWidget *mode_pill;
    GtkWidget *lb_switch;
    GtkWidget *bonding_switch;
    GtkWidget *daemon_banner;
    GtkWidget *devices_box;
    MultiWifiInventory *inventory;
    MultiWifiPathMonitor *path_monitor;
    guint bus_watcher_id;
    GDBusConnection *system_bus;
    GBusType bus_type;
    gboolean daemon_connected;
    gboolean lb_active;
    gboolean bonding_active;
    gboolean updating_switches;
    gboolean daemon_action_pending;
    GPid spawned_daemon_pid;
};

G_DEFINE_TYPE(MultiWifiWindow, multiwifi_window, GTK_TYPE_APPLICATION_WINDOW)

typedef struct {
    MultiWifiWindow *main_win;
    GtkWidget *dialog_window;
    gchar *ifname;
    GtkWidget *list_box;
    GtkWidget *status_lbl;
    GtkWidget *spinner;
    GtkWidget *password_box;
    GtkWidget *password_entry;
    GtkWidget *connect_btn;
    GtkWidget *scan_btn;
    gchar *selected_ssid;
    gboolean selected_is_saved;
    gboolean selected_is_open;
    GCancellable *cancellable;
} WifiSelectDialog;

static void update_devices_ui(MultiWifiWindow *self);
static void update_control_buttons_state(MultiWifiWindow *self);
static gboolean on_lb_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data);
static gboolean on_bonding_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data);
static void on_daemon_toggle_clicked(GtkButton *btn, gpointer user_data);
static void on_about_clicked(GtkButton *btn, gpointer user_data);
static void on_diagnostics_clicked(GtkButton *btn, gpointer user_data);
static void on_select_wifi_clicked(GtkButton *btn, gpointer user_data);
static void on_inventory_devices_changed(MultiWifiInventory *inv, gpointer user_data);
static void on_paths_changed(MultiWifiPathMonitor *mon, gpointer user_data);
static void on_status_query_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void start_daemon_via_pkexec(MultiWifiWindow *self);
static void stop_daemon_via_pkexec(MultiWifiWindow *self);
static void start_daemon_via_session(MultiWifiWindow *self);
static void on_spawned_daemon_exit(GPid pid, gint status, gpointer user_data);

static GBusType s_default_gui_bus_type = G_BUS_TYPE_SYSTEM;

void multiwifi_window_set_default_bus_type(GBusType bus_type)
{
    s_default_gui_bus_type = bus_type;
}

static void multiwifi_window_apply_css(void)
{
    static gboolean applied = FALSE;
    if (applied) {
        return;
    }
    applied = TRUE;

    GtkCssProvider *provider = gtk_css_provider_new();
    GFile *css_file = g_file_new_for_path("data/style.css");
    if (g_file_query_exists(css_file, NULL)) {
        gtk_css_provider_load_from_file(provider, css_file);
    } else {
        gchar *combined = g_strconcat(CSS_FALLBACK_PART1, CSS_FALLBACK_PART2, NULL);
        gtk_css_provider_load_from_string(provider, combined);
        g_free(combined);
    }
    g_object_unref(css_file);

    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }
    g_object_unref(provider);
}

static void multiwifi_window_setup_icons(void)
{
    static gboolean initialized = FALSE;
    if (initialized) {
        return;
    }
    initialized = TRUE;

    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL) {
        return;
    }

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    if (theme == NULL) {
        return;
    }

    /* Add relative search path from current working directory */
    gtk_icon_theme_add_search_path(theme, "data/icons");

    /* Add search paths relative to executable location */
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL) {
        gchar *bindir = g_path_get_dirname(exe);
        gchar *sharedir = g_build_filename(bindir, "..", "share", "icons", NULL);
        gchar *datadir = g_build_filename(bindir, "..", "data", "icons", NULL);
        gtk_icon_theme_add_search_path(theme, sharedir);
        gtk_icon_theme_add_search_path(theme, datadir);
        g_free(datadir);
        g_free(sharedir);
        g_free(bindir);
        g_free(exe);
    }
}

static gchar *find_daemon_executable(void)
{
    gchar *path = g_find_program_in_path("multiwifi-daemon");
    if (path != NULL) {
        return path;
    }

    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe != NULL) {
        gchar *dir = g_path_get_dirname(exe);
        gchar *candidate = g_build_filename(dir, "multiwifi-daemon", NULL);
        g_free(dir);
        g_free(exe);
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
            return candidate;
        }
        g_free(candidate);
    }

    return g_strdup("multiwifi-daemon");
}

static void wifi_select_dialog_free(WifiSelectDialog *dlg)
{
    if (dlg == NULL) {
        return;
    }
    if (dlg->cancellable != NULL) {
        g_cancellable_cancel(dlg->cancellable);
        g_object_unref(dlg->cancellable);
    }
    g_free(dlg->ifname);
    g_free(dlg->selected_ssid);
    g_free(dlg);
}

static void on_wifi_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    if (row == NULL) {
        gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
        return;
    }

    const gchar *ssid = g_object_get_data(G_OBJECT(row), "ssid");
    gboolean is_saved = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "is_saved"));
    gboolean is_open = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "is_open"));

    g_free(dlg->selected_ssid);
    dlg->selected_ssid = g_strdup(ssid);
    dlg->selected_is_saved = is_saved;
    dlg->selected_is_open = is_open;

    if (is_saved) {
        gtk_widget_set_visible(dlg->password_box, FALSE);
        gtk_button_set_label(GTK_BUTTON(dlg->connect_btn), "Connect");
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
    } else if (is_open) {
        gtk_widget_set_visible(dlg->password_box, FALSE);
        gtk_button_set_label(GTK_BUTTON(dlg->connect_btn), "Connect");
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
    } else {
        gtk_widget_set_visible(dlg->password_box, TRUE);
        gtk_button_set_label(GTK_BUTTON(dlg->connect_btn), "Connect");
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
        gtk_widget_grab_focus(dlg->password_entry);
    }
}

static void populate_wifi_list(WifiSelectDialog *dlg)
{
    GtkWidget *child = gtk_widget_get_first_child(dlg->list_box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(dlg->list_box), child);
        child = next;
    }

    GPtrArray *aps = multiwifi_inventory_dup_access_points(dlg->main_win->inventory, dlg->ifname);
    if (aps == NULL || aps->len == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new("No Wi-Fi networks found. Click 'Rescan'.");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_set_margin_bottom(lbl, 16);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_append(GTK_LIST_BOX(dlg->list_box), row);
        if (aps != NULL) {
            g_ptr_array_unref(aps);
        }
        return;
    }

    for (guint i = 0; i < aps->len; i++) {
        MultiWifiAccessPointInfo *ap = g_ptr_array_index(aps, i);

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);
        gtk_widget_set_margin_start(hbox, 10);
        gtk_widget_set_margin_end(hbox, 10);

        const gchar *bars = (ap->signal_strength >= 75) ? "▂▄▆█" :
                            (ap->signal_strength >= 50) ? "▂▄▆_" :
                            (ap->signal_strength >= 25) ? "▂▄__" : "▂___";

        GtkWidget *sig_lbl = gtk_label_new(NULL);
        gchar *sig_markup = g_strdup_printf(
            "<span font_family='monospace' size='small' weight='bold' color='%s'>%s</span>",
            (ap->signal_strength >= 50) ? "#a6e3a1" : "#f9e2af", bars
        );
        gtk_label_set_markup(GTK_LABEL(sig_lbl), sig_markup);
        gtk_box_append(GTK_BOX(hbox), sig_lbl);
        g_free(sig_markup);

        gchar *escaped_ssid = g_markup_escape_text(ap->ssid ? ap->ssid : "Hidden", -1);
        GtkWidget *name_lbl = gtk_label_new(NULL);
        gchar *name_markup = g_strdup_printf("<span weight='bold'>%s</span>", escaped_ssid);
        gtk_label_set_markup(GTK_LABEL(name_lbl), name_markup);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(name_lbl, TRUE);
        gtk_box_append(GTK_BOX(hbox), name_lbl);
        g_free(name_markup);
        g_free(escaped_ssid);

        if (ap->is_active) {
            GtkWidget *act_lbl = gtk_label_new("Connected");
            gtk_widget_add_css_class(act_lbl, "badge");
            gtk_widget_add_css_class(act_lbl, "badge-online");
            gtk_box_append(GTK_BOX(hbox), act_lbl);
        } else if (ap->is_saved) {
            GtkWidget *sav_lbl = gtk_label_new("Saved");
            gtk_widget_add_css_class(sav_lbl, "badge");
            gtk_widget_add_css_class(sav_lbl, "badge-saved");
            gtk_box_append(GTK_BOX(hbox), sav_lbl);
        }

        double freq_ghz = (double)ap->frequency / 1000.0;
        gchar *freq_str = g_strdup_printf("%.1fG", freq_ghz);
        GtkWidget *freq_lbl = gtk_label_new(freq_str);
        gtk_widget_add_css_class(freq_lbl, "badge");
        gtk_widget_add_css_class(freq_lbl, "badge-active");
        gtk_box_append(GTK_BOX(hbox), freq_lbl);
        g_free(freq_str);

        GtkWidget *sec_lbl = gtk_label_new(g_strcmp0(ap->security_str, "Open") == 0 ? "🔓" : "🔒");
        gtk_box_append(GTK_BOX(hbox), sec_lbl);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

        g_object_set_data_full(G_OBJECT(row), "ssid", g_strdup(ap->ssid), g_free);
        g_object_set_data(G_OBJECT(row), "is_saved", GINT_TO_POINTER(ap->is_saved));
        g_object_set_data(G_OBJECT(row), "is_open", GINT_TO_POINTER(g_strcmp0(ap->security_str, "Open") == 0));

        gtk_list_box_append(GTK_LIST_BOX(dlg->list_box), row);
    }

    g_ptr_array_unref(aps);
}

static void on_dialog_scan_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    GError *error = NULL;

    multiwifi_inventory_request_scan_finish(dlg->main_win->inventory, res, &error);
    gtk_spinner_stop(GTK_SPINNER(dlg->spinner));
    gtk_widget_set_sensitive(dlg->scan_btn, TRUE);
    gtk_widget_set_sensitive(dlg->list_box, TRUE);

    if (error != NULL) {
        g_clear_error(&error);
    }

    populate_wifi_list(dlg);
    gtk_label_set_text(GTK_LABEL(dlg->status_lbl), "");
}

static void on_dialog_scan_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    gtk_widget_set_sensitive(dlg->scan_btn, FALSE);
    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    gtk_spinner_start(GTK_SPINNER(dlg->spinner));
    gtk_label_set_markup(GTK_LABEL(dlg->status_lbl),
        "<span size='small' color='#89b4fa'>Scanning networks...</span>");

    multiwifi_inventory_request_scan_async(
        dlg->main_win->inventory,
        dlg->ifname,
        dlg->cancellable,
        on_dialog_scan_ready,
        dlg
    );
}

static void on_dialog_connect_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    GError *error = NULL;

    gboolean ok = multiwifi_inventory_connect_wifi_finish(dlg->main_win->inventory, res, &error);
    gtk_spinner_stop(GTK_SPINNER(dlg->spinner));

    if (ok) {
        gtk_window_destroy(GTK_WINDOW(dlg->dialog_window));
    } else {
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
        gtk_widget_set_sensitive(dlg->scan_btn, TRUE);
        gtk_widget_set_sensitive(dlg->list_box, TRUE);

        gchar *escaped_msg = g_markup_escape_text(error ? error->message : "Failed to connect", -1);
        gchar *err_markup = g_strdup_printf("<span size='small' color='#f38ba8'>%s</span>", escaped_msg);
        gtk_label_set_markup(GTK_LABEL(dlg->status_lbl), err_markup);
        g_free(err_markup);
        g_free(escaped_msg);
        g_clear_error(&error);
    }
}

static void on_dialog_connect_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    if (dlg->selected_ssid == NULL) {
        return;
    }

    const gchar *pass = NULL;
    if (gtk_widget_get_visible(dlg->password_box)) {
        pass = gtk_editable_get_text(GTK_EDITABLE(dlg->password_entry));
    }

    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    gtk_widget_set_sensitive(dlg->scan_btn, FALSE);
    gtk_widget_set_sensitive(dlg->list_box, FALSE);
    gtk_spinner_start(GTK_SPINNER(dlg->spinner));

    gchar *escaped_ssid = g_markup_escape_text(dlg->selected_ssid, -1);
    gchar *status_markup = g_strdup_printf(
        "<span size='small' color='#89b4fa'>Connecting to '%s'...</span>", escaped_ssid
    );
    gtk_label_set_markup(GTK_LABEL(dlg->status_lbl), status_markup);
    g_free(status_markup);
    g_free(escaped_ssid);

    multiwifi_inventory_connect_wifi_async(
        dlg->main_win->inventory,
        dlg->ifname,
        dlg->selected_ssid,
        pass,
        dlg->cancellable,
        on_dialog_connect_ready,
        dlg
    );
}

static void on_select_wifi_clicked(GtkButton *btn, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    const gchar *ifname = g_object_get_data(G_OBJECT(btn), "ifname");
    if (ifname == NULL || self->inventory == NULL) {
        return;
    }

    WifiSelectDialog *dlg = g_new0(WifiSelectDialog, 1);
    dlg->main_win = self;
    dlg->ifname = g_strdup(ifname);
    dlg->cancellable = g_cancellable_new();

    dlg->dialog_window = gtk_window_new();
    gtk_widget_add_css_class(dlg->dialog_window, "main-window");

    gchar *win_title = g_strdup_printf("Wi-Fi Networks (%s)", ifname);
    gtk_window_set_title(GTK_WINDOW(dlg->dialog_window), win_title);
    g_free(win_title);

    gtk_window_set_default_size(GTK_WINDOW(dlg->dialog_window), 460, 480);
    gtk_window_set_transient_for(GTK_WINDOW(dlg->dialog_window), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(dlg->dialog_window), TRUE);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dlg->dialog_window), header);

    dlg->spinner = gtk_spinner_new();
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), dlg->spinner);

    dlg->scan_btn = gtk_button_new_with_label("↻ Rescan");
    gtk_widget_add_css_class(dlg->scan_btn, "btn-secondary");
    g_signal_connect(dlg->scan_btn, "clicked", G_CALLBACK(on_dialog_scan_clicked), dlg);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), dlg->scan_btn);

    /* Main Container */
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content_box, 14);
    gtk_widget_set_margin_bottom(content_box, 14);
    gtk_widget_set_margin_start(content_box, 16);
    gtk_widget_set_margin_end(content_box, 16);

    /* Scrolled list */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);

    dlg->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dlg->list_box), GTK_SELECTION_SINGLE);
    g_signal_connect(dlg->list_box, "row-selected", G_CALLBACK(on_wifi_row_selected), dlg);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dlg->list_box);
    gtk_box_append(GTK_BOX(content_box), scrolled);

    /* Password Input Box (Hidden initially) */
    dlg->password_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_visible(dlg->password_box, FALSE);

    dlg->password_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(dlg->password_entry), TRUE);
    gtk_widget_set_hexpand(dlg->password_entry, TRUE);
    gtk_box_append(GTK_BOX(dlg->password_box), dlg->password_entry);
    gtk_box_append(GTK_BOX(content_box), dlg->password_box);

    /* Status Label */
    dlg->status_lbl = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(dlg->status_lbl), TRUE);
    gtk_box_append(GTK_BOX(content_box), dlg->status_lbl);

    /* Bottom Button Row */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel_btn, "btn-secondary");
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_destroy), dlg->dialog_window);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    dlg->connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(dlg->connect_btn, "btn-primary");
    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    g_signal_connect(dlg->connect_btn, "clicked", G_CALLBACK(on_dialog_connect_clicked), dlg);
    g_signal_connect(dlg->password_entry, "activate", G_CALLBACK(on_dialog_connect_clicked), dlg);
    gtk_box_append(GTK_BOX(btn_row), dlg->connect_btn);

    gtk_box_append(GTK_BOX(content_box), btn_row);

    gtk_window_set_child(GTK_WINDOW(dlg->dialog_window), content_box);

    g_signal_connect_swapped(dlg->dialog_window, "destroy", G_CALLBACK(wifi_select_dialog_free), dlg);

    populate_wifi_list(dlg);
    gtk_window_present(GTK_WINDOW(dlg->dialog_window));
}

static void update_devices_ui(MultiWifiWindow *self)
{
    if (self->devices_box == NULL || self->inventory == NULL) {
        return;
    }

    GtkWidget *child = gtk_widget_get_first_child(self->devices_box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(self->devices_box), child);
        child = next;
    }

    GPtrArray *devices = multiwifi_inventory_dup_devices(self->inventory);
    if (devices == NULL || devices->len == 0) {
        GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(empty_box, "modern-card");

        GtkWidget *empty_label = gtk_label_new("No Wi-Fi adapters detected.");
        gtk_widget_add_css_class(empty_label, "badge-offline");
        gtk_box_append(GTK_BOX(empty_box), empty_label);
        gtk_box_append(GTK_BOX(self->devices_box), empty_box);

        if (devices != NULL) {
            g_ptr_array_unref(devices);
        }
        return;
    }

    for (guint i = 0; i < devices->len; i++) {
        MultiWifiDeviceInfo *info = g_ptr_array_index(devices, i);
        MultiWifiPathInfo *path = self->path_monitor ?
            multiwifi_path_monitor_find_path(self->path_monitor, info->ifname) : NULL;

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(card_box, "modern-card");

        /* Row 1: Header (Icon, Interface, Chip Type, Status Badge) */
        GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        gboolean is_usb = (info->ifname && strstr(info->ifname, "u") != NULL);
        const gchar *hw_icon = is_usb ? "🔌" : "💻";
        const gchar *type_desc = is_usb ? "USB" : "PCIe";

        gchar *title_markup = g_strdup_printf(
            "<span size='medium' weight='bold'>%s %s</span>",
            hw_icon, info->ifname ? info->ifname : "unknown"
        );
        GtkWidget *title_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(title_lbl), title_markup);
        gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(header_row), title_lbl);
        g_free(title_markup);

        GtkWidget *hw_badge = gtk_label_new(type_desc);
        gtk_widget_add_css_class(hw_badge, "badge");
        gtk_widget_add_css_class(hw_badge, "badge-hw");
        gtk_box_append(GTK_BOX(header_row), hw_badge);

        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(header_row), spacer);

        GtkWidget *badge_lbl = gtk_label_new(NULL);
        gtk_widget_add_css_class(badge_lbl, "badge");
        if (path != NULL) {
            if (path->state == MULTIWIFI_PATH_STATE_ONLINE) {
                gtk_label_set_text(GTK_LABEL(badge_lbl), "● Online");
                gtk_widget_add_css_class(badge_lbl, "badge-online");
            } else if (path->state == MULTIWIFI_PATH_STATE_DEGRADED) {
                gtk_label_set_text(GTK_LABEL(badge_lbl), "▲ Degraded");
                gtk_widget_add_css_class(badge_lbl, "badge-degraded");
            } else if (path->state == MULTIWIFI_PATH_STATE_LOCAL_ONLY) {
                gtk_label_set_text(GTK_LABEL(badge_lbl), "✕ Local Only");
                gtk_widget_add_css_class(badge_lbl, "badge-danger");
            } else if (path->state == MULTIWIFI_PATH_STATE_PROBING) {
                gtk_label_set_text(GTK_LABEL(badge_lbl), "◌ Probing");
                gtk_widget_add_css_class(badge_lbl, "badge-active");
            } else {
                gtk_label_set_text(GTK_LABEL(badge_lbl), "○ Offline");
                gtk_widget_add_css_class(badge_lbl, "badge-offline");
            }
        } else if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            gtk_label_set_text(GTK_LABEL(badge_lbl), "● Connected");
            gtk_widget_add_css_class(badge_lbl, "badge-online");
        } else {
            gtk_label_set_text(GTK_LABEL(badge_lbl), "○ Idle");
            gtk_widget_add_css_class(badge_lbl, "badge-offline");
        }
        gtk_box_append(GTK_BOX(header_row), badge_lbl);
        gtk_box_append(GTK_BOX(card_box), header_row);

        /* Row 2: Wi-Fi SSID & Switch Action */
        GtkWidget *mid_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_valign(mid_row, GTK_ALIGN_CENTER);

        if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            GtkWidget *ssid_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(ssid_box, TRUE);

            GtkWidget *top_ssid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gchar *escaped_ssid = g_markup_escape_text(info->ssid ? info->ssid : "Hidden", -1);
            gchar *ssid_markup = g_strdup_printf("<span size='large' weight='bold'>📶 %s</span>", escaped_ssid);
            GtkWidget *ssid_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(ssid_lbl), ssid_markup);
            gtk_box_append(GTK_BOX(top_ssid), ssid_lbl);
            g_free(ssid_markup);
            g_free(escaped_ssid);

            double freq_ghz = (double)info->frequency / 1000.0;
            gchar *freq_str = g_strdup_printf("%.1f GHz", freq_ghz);
            GtkWidget *freq_badge = gtk_label_new(freq_str);
            gtk_widget_add_css_class(freq_badge, "badge");
            gtk_widget_add_css_class(freq_badge, "badge-active");
            gtk_box_append(GTK_BOX(top_ssid), freq_badge);
            g_free(freq_str);

            const gchar *bars = (info->signal_strength >= 75) ? "▂▄▆█" :
                                (info->signal_strength >= 50) ? "▂▄▆_" :
                                (info->signal_strength >= 25) ? "▂▄__" : "▂___";
            gchar *sig_str = g_strdup_printf("%s %u%%", bars, info->signal_strength);
            GtkWidget *sig_badge = gtk_label_new(sig_str);
            gtk_widget_add_css_class(sig_badge, "badge");
            gtk_widget_add_css_class(sig_badge, (info->signal_strength >= 50) ? "badge-online" : "badge-degraded");
            gtk_box_append(GTK_BOX(top_ssid), sig_badge);
            g_free(sig_str);

            gtk_box_append(GTK_BOX(ssid_box), top_ssid);

            gchar *ip_markup = g_strdup_printf(
                "<span size='small' color='#6c7086'>IP: <tt>%s</tt></span>",
                info->ipv4_address ? info->ipv4_address : "Pending"
            );
            GtkWidget *ip_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(ip_lbl), ip_markup);
            gtk_label_set_xalign(GTK_LABEL(ip_lbl), 0.0f);
            gtk_box_append(GTK_BOX(ssid_box), ip_lbl);
            g_free(ip_markup);

            gtk_box_append(GTK_BOX(mid_row), ssid_box);
        } else {
            GtkWidget *idle_lbl = gtk_label_new("Not connected");
            gtk_widget_set_hexpand(idle_lbl, TRUE);
            gtk_widget_set_halign(idle_lbl, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(mid_row), idle_lbl);
        }

        const gchar *btn_label = (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) ? "Switch Wi-Fi" : "Connect";
        GtkWidget *wifi_btn = gtk_button_new_with_label(btn_label);
        gtk_widget_add_css_class(wifi_btn, "btn-secondary");
        g_object_set_data_full(G_OBJECT(wifi_btn), "ifname", g_strdup(info->ifname), g_free);
        g_signal_connect(wifi_btn, "clicked", G_CALLBACK(on_select_wifi_clicked), self);
        gtk_box_append(GTK_BOX(mid_row), wifi_btn);

        gtk_box_append(GTK_BOX(card_box), mid_row);

        /* Row 3: Metric Chips */
        if (path != NULL && path->state != MULTIWIFI_PATH_STATE_OFFLINE) {
            GtkWidget *chips_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(chips_row, 4);

            gchar *rtt_str = g_strdup_printf("⚡ %.1f ms", path->rtt_ms);
            GtkWidget *rtt_chip = gtk_label_new(rtt_str);
            gtk_widget_add_css_class(rtt_chip, "metric-chip");
            gtk_box_append(GTK_BOX(chips_row), rtt_chip);
            g_free(rtt_str);

            gchar *loss_str = g_strdup_printf("📉 %u%% Loss", path->packet_loss_percent);
            GtkWidget *loss_chip = gtk_label_new(loss_str);
            gtk_widget_add_css_class(loss_chip, "metric-chip");
            gtk_box_append(GTK_BOX(chips_row), loss_chip);
            g_free(loss_str);

            const gchar *steer_str = path->is_eligible ? "🔀 Active Link" : "Standby";
            GtkWidget *steer_chip = gtk_label_new(steer_str);
            gtk_widget_add_css_class(steer_chip, "metric-chip");
            gtk_box_append(GTK_BOX(chips_row), steer_chip);

            gtk_box_append(GTK_BOX(card_box), chips_row);
        }

        if (path != NULL) {
            multiwifi_path_info_free(path);
        }

        gtk_box_append(GTK_BOX(self->devices_box), card_box);
    }

    g_ptr_array_unref(devices);
}

static void on_inventory_devices_changed(MultiWifiInventory *inv, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);

    if (self->path_monitor != NULL && inv != NULL) {
        GPtrArray *devices = multiwifi_inventory_dup_devices(inv);
        if (devices != NULL) {
            for (guint i = 0; i < devices->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devices, i);
                multiwifi_path_monitor_update_device(self->path_monitor, d);
            }
            g_ptr_array_unref(devices);
        }
    }

    update_devices_ui(self);
}

static void on_paths_changed(MultiWifiPathMonitor *mon, gpointer user_data)
{
    (void)mon;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    update_devices_ui(self);
}

static void update_control_buttons_state(MultiWifiWindow *self)
{
    if (self->lb_switch == NULL || self->bonding_switch == NULL || self->daemon_banner == NULL) {
        return;
    }

    self->updating_switches = TRUE;
    gtk_switch_set_active(GTK_SWITCH(self->lb_switch), self->lb_active);
    gtk_switch_set_active(GTK_SWITCH(self->bonding_switch), self->bonding_active);
    self->updating_switches = FALSE;

    /* Update Daemon Start / Stop Button */
    if (self->daemon_toggle_btn != NULL) {
        gtk_widget_set_sensitive(self->daemon_toggle_btn, !self->daemon_action_pending);
        if (self->daemon_connected) {
            gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "⏹ Stop Daemon");
            gtk_widget_remove_css_class(self->daemon_toggle_btn, "btn-primary");
            gtk_widget_add_css_class(self->daemon_toggle_btn, "btn-secondary");
            gtk_widget_set_tooltip_text(self->daemon_toggle_btn, "Stop the MultiWiFi system service");
        } else {
            gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "🚀 Start Daemon");
            gtk_widget_remove_css_class(self->daemon_toggle_btn, "btn-secondary");
            gtk_widget_add_css_class(self->daemon_toggle_btn, "btn-primary");
            gtk_widget_set_tooltip_text(self->daemon_toggle_btn, "Start MultiWiFi system daemon via Polkit / systemd");
        }
    }

    if (self->daemon_connected) {
        if (self->daemon_status_lbl != NULL) {
            gtk_widget_remove_css_class(self->daemon_status_lbl, "badge-offline");
            gtk_widget_add_css_class(self->daemon_status_lbl, "badge-online");
            gtk_label_set_text(GTK_LABEL(self->daemon_status_lbl), "● Daemon Active");
        }

        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#a6e3a1'>✔ Daemon active on System D-Bus</span>");
        gtk_widget_set_sensitive(self->lb_switch, !self->bonding_active);
        gtk_widget_set_sensitive(self->bonding_switch, !self->lb_active);

        if (self->mode_pill != NULL) {
            if (self->bonding_active) {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "MPTCP Bonding");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_remove_css_class(self->mode_pill, "badge-online");
                gtk_widget_add_css_class(self->mode_pill, "badge-active");
            } else if (self->lb_active) {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "Load Balancing");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_remove_css_class(self->mode_pill, "badge-online");
                gtk_widget_add_css_class(self->mode_pill, "badge-active");
            } else {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "Dual-Link Active");
                gtk_widget_remove_css_class(self->mode_pill, "badge-active");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_add_css_class(self->mode_pill, "badge-online");
            }
        }
    } else {
        if (self->daemon_status_lbl != NULL) {
            gtk_widget_remove_css_class(self->daemon_status_lbl, "badge-online");
            gtk_widget_add_css_class(self->daemon_status_lbl, "badge-offline");
            gtk_label_set_text(GTK_LABEL(self->daemon_status_lbl), "○ Standalone");
        }

        if (!self->daemon_action_pending) {
            gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
                "<span size='small' color='#f38ba8'>⚠ Daemon offline • Routing controls disabled</span>");
        }
        gtk_widget_set_sensitive(self->lb_switch, FALSE);
        gtk_widget_set_sensitive(self->bonding_switch, FALSE);

        if (self->mode_pill != NULL) {
            gtk_label_set_text(GTK_LABEL(self->mode_pill), "Standalone");
            gtk_widget_remove_css_class(self->mode_pill, "badge-active");
            gtk_widget_remove_css_class(self->mode_pill, "badge-online");
            gtk_widget_add_css_class(self->mode_pill, "badge-offline");
        }
    }
}

static void stop_daemon_via_pkexec(MultiWifiWindow *self)
{
    const gchar *argv[] = { "pkexec", "pkill", "-TERM", "-x", "multiwifi-daemon", NULL };
    GError *spawn_err = NULL;

    gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
        "<span size='small' color='#89b4fa'>Prompting authorization to stop daemon...</span>");

    gboolean ok = g_spawn_async(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        &spawn_err
    );

    self->daemon_action_pending = FALSE;

    if (!ok) {
        gchar *escaped = g_markup_escape_text(spawn_err ? spawn_err->message : "Stop failed", -1);
        gchar *err_msg = g_strdup_printf(
            "<span size='small' color='#f38ba8'>Failed to stop daemon: %s</span>", escaped
        );
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner), err_msg);
        g_free(err_msg);
        g_free(escaped);
        g_clear_error(&spawn_err);
    }

    update_control_buttons_state(self);
}

static void on_spawned_daemon_exit(GPid pid, gint status, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    g_spawn_close_pid(pid);
    if (self->spawned_daemon_pid == pid) {
        self->spawned_daemon_pid = 0;
    }
    self->daemon_action_pending = FALSE;

    if (!self->daemon_connected) {
        gchar *err_msg = NULL;
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            err_msg = g_strdup_printf(
                "<span size='small' color='#f38ba8'>Daemon exited (code %d). For system daemon run: sudo cmake --install build</span>",
                WEXITSTATUS(status)
            );
        } else {
            err_msg = g_strdup(
                "<span size='small' color='#f38ba8'>Authorization cancelled or service not installed. Run: sudo cmake --install build</span>"
            );
        }
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner), err_msg);
        g_free(err_msg);
    }

    update_control_buttons_state(self);
}

static void start_daemon_via_session(MultiWifiWindow *self)
{
    gchar *daemon_path = find_daemon_executable();
    const gchar *argv[] = { daemon_path, "--session", "--no-polkit", NULL };
    GError *spawn_err = NULL;

    gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
        "<span size='small' color='#89b4fa'>Starting local test daemon on session bus...</span>");

    self->daemon_action_pending = TRUE;
    gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "Starting...");
    gtk_widget_set_sensitive(self->daemon_toggle_btn, FALSE);

    gboolean ok = g_spawn_async(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,
        NULL,
        &self->spawned_daemon_pid,
        &spawn_err
    );
    g_free(daemon_path);

    if (ok) {
        g_child_watch_add(self->spawned_daemon_pid, on_spawned_daemon_exit, self);
    } else {
        self->daemon_action_pending = FALSE;
        gchar *escaped = g_markup_escape_text(spawn_err ? spawn_err->message : "Failed to spawn daemon", -1);
        gchar *err_msg = g_strdup_printf(
            "<span size='small' color='#f38ba8'>Failed to start: %s</span>", escaped
        );
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner), err_msg);
        g_free(err_msg);
        g_free(escaped);
        g_clear_error(&spawn_err);
        update_control_buttons_state(self);
    }
}

static void start_daemon_via_pkexec(MultiWifiWindow *self)
{
    gchar *daemon_path = find_daemon_executable();
    const gchar *argv[] = { "pkexec", daemon_path, NULL };
    GError *spawn_err = NULL;

    gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
        "<span size='small' color='#89b4fa'>Prompting administrator authorization (Polkit)...</span>");

    self->daemon_action_pending = TRUE;
    gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "Starting...");
    gtk_widget_set_sensitive(self->daemon_toggle_btn, FALSE);

    gboolean ok = g_spawn_async(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,
        NULL,
        &self->spawned_daemon_pid,
        &spawn_err
    );
    g_free(daemon_path);

    if (ok) {
        g_child_watch_add(self->spawned_daemon_pid, on_spawned_daemon_exit, self);
    } else {
        self->daemon_action_pending = FALSE;
        gchar *escaped = g_markup_escape_text(spawn_err ? spawn_err->message : "Authorization failed", -1);
        gchar *err_msg = g_strdup_printf(
            "<span size='small' color='#f38ba8'>Polkit failed: %s</span>", escaped
        );
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner), err_msg);
        g_free(err_msg);
        g_free(escaped);
        g_clear_error(&spawn_err);
        update_control_buttons_state(self);
    }
}

static void on_daemon_systemd_stop_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_variant_unref(reply);
        self->daemon_action_pending = FALSE;
        update_control_buttons_state(self);
    } else {
        g_debug("systemd StopUnit failed (%s), attempting pkexec fallback", error ? error->message : "unknown");
        g_clear_error(&error);
        stop_daemon_via_pkexec(self);
    }
}

static void on_daemon_systemd_start_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_variant_unref(reply);
        self->daemon_action_pending = FALSE;
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#89b4fa'>Authenticating &amp; starting daemon...</span>");
        update_control_buttons_state(self);
    } else {
        g_debug("systemd StartUnit failed (%s), attempting pkexec fallback", error ? error->message : "unknown");
        g_clear_error(&error);
        start_daemon_via_pkexec(self);
    }
}

static void on_daemon_toggle_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (self->daemon_action_pending) {
        return;
    }

    if (self->daemon_connected) {
        self->daemon_action_pending = TRUE;
        gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "Stopping...");
        gtk_widget_set_sensitive(self->daemon_toggle_btn, FALSE);

        if (self->spawned_daemon_pid > 0) {
            kill(self->spawned_daemon_pid, SIGTERM);
            g_spawn_close_pid(self->spawned_daemon_pid);
            self->spawned_daemon_pid = 0;
        }

        if (self->bus_type == G_BUS_TYPE_SESSION) {
            self->daemon_action_pending = FALSE;
            update_control_buttons_state(self);
            return;
        }

        if (self->system_bus != NULL) {
            g_dbus_connection_call(
                self->system_bus,
                "org.freedesktop.systemd1",
                "/org/freedesktop/systemd1",
                "org.freedesktop.systemd1.Manager",
                "StopUnit",
                g_variant_new("(ss)", "multiwifi-daemon.service", "replace"),
                G_VARIANT_TYPE("(o)"),
                G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                15000,
                NULL,
                on_daemon_systemd_stop_done,
                self
            );
        } else {
            self->daemon_action_pending = FALSE;
            update_control_buttons_state(self);
        }
    } else {
        if (self->bus_type == G_BUS_TYPE_SESSION) {
            start_daemon_via_session(self);
            return;
        }

        self->daemon_action_pending = TRUE;
        gtk_button_set_label(GTK_BUTTON(self->daemon_toggle_btn), "Starting...");
        gtk_widget_set_sensitive(self->daemon_toggle_btn, FALSE);

        if (self->system_bus != NULL) {
            g_dbus_connection_call(
                self->system_bus,
                "org.freedesktop.systemd1",
                "/org/freedesktop/systemd1",
                "org.freedesktop.systemd1.Manager",
                "StartUnit",
                g_variant_new("(ss)", "multiwifi-daemon.service", "replace"),
                G_VARIANT_TYPE("(o)"),
                G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                25000,
                NULL,
                on_daemon_systemd_start_done,
                self
            );
        } else {
            start_daemon_via_pkexec(self);
        }
    }
}

static void on_status_query_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (reply != NULL) {
        GVariant *dict = NULL;
        g_variant_get(reply, "(@a{sv})", &dict);
        if (dict != NULL) {
            GVariant *v_mode = g_variant_lookup_value(dict, "mode", G_VARIANT_TYPE_STRING);
            if (v_mode != NULL) {
                const gchar *m = g_variant_get_string(v_mode, NULL);
                self->lb_active = (g_strcmp0(m, "load_balancing") == 0);
                self->bonding_active = (g_strcmp0(m, "bonding") == 0);
                g_variant_unref(v_mode);
            }
            g_variant_unref(dict);
        }
        g_variant_unref(reply);
    } else if (error != NULL) {
        g_clear_error(&error);
    }
    update_control_buttons_state(self);
}

static void on_lb_action_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
        self->lb_active = !self->lb_active;
    } else if (error != NULL) {
        g_warning("Load balancing action failed: %s", error->message);
        g_clear_error(&error);
    }
    update_control_buttons_state(self);
}

static gboolean on_lb_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void)sw;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (self->updating_switches) {
        return FALSE;
    }
    if (!self->daemon_connected || self->system_bus == NULL) {
        return TRUE;
    }

    if (state && !self->lb_active) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_dbus_connection_call(
            self->system_bus,
            MULTIWIFI_DBUS_NAME,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "StartLoadBalancing",
            g_variant_new("(@a{sv})", g_variant_builder_end(&b)),
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000,
            NULL,
            on_lb_action_done,
            self
        );
    } else if (!state && self->lb_active) {
        g_dbus_connection_call(
            self->system_bus,
            MULTIWIFI_DBUS_NAME,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "StopLoadBalancing",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000,
            NULL,
            on_lb_action_done,
            self
        );
    }
    return FALSE;
}

static void on_bonding_action_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
        self->bonding_active = !self->bonding_active;
    } else if (error != NULL) {
        g_warning("Bonding action failed: %s", error->message);
        g_clear_error(&error);
    }
    update_control_buttons_state(self);
}

static gboolean on_bonding_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data)
{
    (void)sw;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (self->updating_switches) {
        return FALSE;
    }
    if (!self->daemon_connected || self->system_bus == NULL) {
        return TRUE;
    }

    if (state && !self->bonding_active) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "server_host", g_variant_new_string("127.0.0.1"));
        g_variant_builder_add(&b, "{sv}", "server_port", g_variant_new_uint32(5000));
        g_variant_builder_add(&b, "{sv}", "token", g_variant_new_string("demo-token"));

        g_dbus_connection_call(
            self->system_bus,
            MULTIWIFI_DBUS_NAME,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "StartBonding",
            g_variant_new("(@a{sv})", g_variant_builder_end(&b)),
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000,
            NULL,
            on_bonding_action_done,
            self
        );
    } else if (!state && self->bonding_active) {
        g_dbus_connection_call(
            self->system_bus,
            MULTIWIFI_DBUS_NAME,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "StopBonding",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            10000,
            NULL,
            on_bonding_action_done,
            self
        );
    }
    return FALSE;
}

static void on_daemon_name_appeared(GDBusConnection *connection,
                                    const gchar *name,
                                    const gchar *name_owner,
                                    gpointer user_data)
{
    (void)name;
    (void)name_owner;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    self->daemon_connected = TRUE;
    self->daemon_action_pending = FALSE;
    if (self->system_bus == NULL) {
        self->system_bus = g_object_ref(connection);
    }

    g_dbus_connection_call(
        self->system_bus,
        MULTIWIFI_DBUS_NAME,
        MULTIWIFI_DBUS_PATH,
        MULTIWIFI_DBUS_INTERFACE,
        "GetStatus",
        NULL,
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        on_status_query_done,
        self
    );

    update_control_buttons_state(self);
}

static void on_daemon_name_vanished(GDBusConnection *connection,
                                    const gchar *name,
                                    gpointer user_data)
{
    (void)connection;
    (void)name;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    self->daemon_connected = FALSE;
    self->lb_active = FALSE;
    self->bonding_active = FALSE;
    self->daemon_action_pending = FALSE;
    update_control_buttons_state(self);
}

static void on_diagnostics_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (self->daemon_connected && self->system_bus != NULL) {
        g_dbus_connection_call(
            self->system_bus,
            MULTIWIFI_DBUS_NAME,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "RunDiagnostics",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            NULL,
            NULL
        );
    }
    if (self->path_monitor != NULL) {
        multiwifi_path_monitor_probe_all_async(self->path_monitor);
    }
}

static void on_about_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);

    GtkWidget *about = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "MultiWiFi");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), MULTIWIFI_VERSION_STRING);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about),
        "High-performance native Linux multi-path Wi-Fi load balancing and MPTCP bonding");
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "io.github.multiwifi.MultiWiFi");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about), "https://github.com/TrVHau/multi-wifi");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about), GTK_LICENSE_MIT_X11);
    const char *authors[] = { "MultiWiFi Team", NULL };
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);

    gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(about), TRUE);
    gtk_window_present(GTK_WINDOW(about));
}

static void multiwifi_window_dispose(GObject *object)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(object);

    if (self->spawned_daemon_pid > 0) {
        kill(self->spawned_daemon_pid, SIGTERM);
        g_spawn_close_pid(self->spawned_daemon_pid);
        self->spawned_daemon_pid = 0;
    }

    if (self->bus_watcher_id > 0) {
        g_bus_unwatch_name(self->bus_watcher_id);
        self->bus_watcher_id = 0;
    }
    g_clear_object(&self->system_bus);

    if (self->path_monitor != NULL) {
        multiwifi_path_monitor_stop(self->path_monitor);
        g_signal_handlers_disconnect_by_data(self->path_monitor, self);
        g_clear_object(&self->path_monitor);
    }

    if (self->inventory != NULL) {
        g_signal_handlers_disconnect_by_data(self->inventory, self);
        g_clear_object(&self->inventory);
    }

    G_OBJECT_CLASS(multiwifi_window_parent_class)->dispose(object);
}

static void multiwifi_window_init(MultiWifiWindow *self)
{
    multiwifi_window_setup_icons();
    multiwifi_window_apply_css();

    gtk_window_set_title(GTK_WINDOW(self), "MultiWiFi");
    gtk_window_set_icon_name(GTK_WINDOW(self), "io.github.multiwifi.MultiWiFi");
    gtk_window_set_default_size(GTK_WINDOW(self), 680, 600);
    gtk_widget_add_css_class(GTK_WIDGET(self), "main-window");

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(self), header);

    /* Centered Brand Title with Logo */
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);

    GtkWidget *logo_img = gtk_image_new_from_icon_name("io.github.multiwifi.MultiWiFi");
    gtk_image_set_pixel_size(GTK_IMAGE(logo_img), 24);
    gtk_box_append(GTK_BOX(title_box), logo_img);

    GtkWidget *title_lbl = gtk_label_new("MultiWiFi");
    gtk_widget_add_css_class(title_lbl, "title");
    gtk_box_append(GTK_BOX(title_box), title_lbl);

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_box);

    /* Left: Daemon Status Badge */
    self->daemon_status_lbl = gtk_label_new("○ Standalone");
    gtk_widget_add_css_class(self->daemon_status_lbl, "badge");
    gtk_widget_add_css_class(self->daemon_status_lbl, "badge-offline");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), self->daemon_status_lbl);

    /* Right: About button */
    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_add_css_class(about_btn, "btn-secondary");
    gtk_widget_set_tooltip_text(about_btn, "About MultiWiFi");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), self);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);

    /* Right: Refresh button */
    GtkWidget *diag_btn = gtk_button_new_with_label("↻ Probe");
    gtk_widget_add_css_class(diag_btn, "btn-secondary");
    gtk_widget_set_tooltip_text(diag_btn, "Probe latency and health across all uplinks");
    g_signal_connect(diag_btn, "clicked", G_CALLBACK(on_diagnostics_clicked), self);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), diag_btn);

    /* Initialize D-Bus Connection (System or Session bus) */
    self->bus_type = s_default_gui_bus_type;
    GError *bus_err = NULL;
    self->system_bus = g_bus_get_sync(self->bus_type, NULL, &bus_err);
    if (self->system_bus == NULL) {
        g_clear_error(&bus_err);
    }

    self->daemon_connected = FALSE;
    self->updating_switches = FALSE;
    self->daemon_action_pending = FALSE;
    self->spawned_daemon_pid = 0;

    self->bus_watcher_id = g_bus_watch_name(
        self->bus_type,
        MULTIWIFI_DBUS_NAME,
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_daemon_name_appeared,
        on_daemon_name_vanished,
        self,
        NULL
    );

    /* Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(main_box, 16);
    gtk_widget_set_margin_bottom(main_box, 16);
    gtk_widget_set_margin_start(main_box, 18);
    gtk_widget_set_margin_end(main_box, 18);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), main_box);
    gtk_window_set_child(GTK_WINDOW(self), scrolled);

    /* 1. Multi-Path Controller Hero Card with Switches & Daemon Toggle */
    GtkWidget *hero_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(hero_card, "hero-card");

    GtkWidget *hero_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *hero_sec_lbl = gtk_label_new("TRAFFIC CONTROLLER");
    gtk_widget_add_css_class(hero_sec_lbl, "section-header");
    gtk_widget_set_halign(hero_sec_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(hero_sec_lbl, TRUE);
    gtk_box_append(GTK_BOX(hero_top), hero_sec_lbl);

    self->mode_pill = gtk_label_new("Dual-Link Active");
    gtk_widget_add_css_class(self->mode_pill, "badge");
    gtk_widget_add_css_class(self->mode_pill, "badge-online");
    gtk_box_append(GTK_BOX(hero_top), self->mode_pill);
    gtk_box_append(GTK_BOX(hero_card), hero_top);

    /* Action Row 1: Load Balancing Switch */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row1, "action-row");

    GtkWidget *icon1 = gtk_label_new("⚡");
    gtk_box_append(GTK_BOX(row1), icon1);

    GtkWidget *box1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t1 = gtk_label_new("Load Balancing");
    gtk_widget_set_halign(t1, GTK_ALIGN_START);
    gchar *t1_markup = g_strdup("<span weight='bold'>Load Balancing</span>");
    gtk_label_set_markup(GTK_LABEL(t1), t1_markup);
    g_free(t1_markup);
    gtk_box_append(GTK_BOX(box1), t1);

    GtkWidget *s1 = gtk_label_new(NULL);
    gtk_widget_set_halign(s1, GTK_ALIGN_START);
    gchar *s1_markup = g_strdup("<span size='small' color='#a6adc8'>Dynamic flow distribution across all Wi-Fi uplinks</span>");
    gtk_label_set_markup(GTK_LABEL(s1), s1_markup);
    g_free(s1_markup);
    gtk_box_append(GTK_BOX(box1), s1);
    gtk_widget_set_hexpand(box1, TRUE);
    gtk_box_append(GTK_BOX(row1), box1);

    self->lb_switch = gtk_switch_new();
    gtk_widget_set_valign(self->lb_switch, GTK_ALIGN_CENTER);
    g_signal_connect(self->lb_switch, "state-set", G_CALLBACK(on_lb_switch_state_set), self);
    gtk_box_append(GTK_BOX(row1), self->lb_switch);
    gtk_box_append(GTK_BOX(hero_card), row1);

    /* Action Row 2: Tunnel Bonding Switch */
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row2, "action-row");

    GtkWidget *icon2 = gtk_label_new("🔗");
    gtk_box_append(GTK_BOX(row2), icon2);

    GtkWidget *box2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *t2 = gtk_label_new(NULL);
    gtk_widget_set_halign(t2, GTK_ALIGN_START);
    gchar *t2_markup = g_strdup("<span weight='bold'>Tunnel Bonding</span>");
    gtk_label_set_markup(GTK_LABEL(t2), t2_markup);
    g_free(t2_markup);
    gtk_box_append(GTK_BOX(box2), t2);

    GtkWidget *s2 = gtk_label_new(NULL);
    gtk_widget_set_halign(s2, GTK_ALIGN_START);
    gchar *s2_markup = g_strdup("<span size='small' color='#a6adc8'>MPTCP tunnel bonding for aggregated single streams</span>");
    gtk_label_set_markup(GTK_LABEL(s2), s2_markup);
    g_free(s2_markup);
    gtk_box_append(GTK_BOX(box2), s2);
    gtk_widget_set_hexpand(box2, TRUE);
    gtk_box_append(GTK_BOX(row2), box2);

    self->bonding_switch = gtk_switch_new();
    gtk_widget_set_valign(self->bonding_switch, GTK_ALIGN_CENTER);
    g_signal_connect(self->bonding_switch, "state-set", G_CALLBACK(on_bonding_switch_state_set), self);
    gtk_box_append(GTK_BOX(row2), self->bonding_switch);
    gtk_box_append(GTK_BOX(hero_card), row2);

    /* Daemon Status & Start / Stop Toggle Row */
    GtkWidget *daemon_ctrl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(daemon_ctrl_row, 4);

    self->daemon_banner = gtk_label_new(NULL);
    gtk_widget_set_hexpand(self->daemon_banner, TRUE);
    gtk_widget_set_halign(self->daemon_banner, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(daemon_ctrl_row), self->daemon_banner);

    self->daemon_toggle_btn = gtk_button_new_with_label("🚀 Start Daemon");
    gtk_widget_add_css_class(self->daemon_toggle_btn, "btn-primary");
    g_signal_connect(self->daemon_toggle_btn, "clicked", G_CALLBACK(on_daemon_toggle_clicked), self);
    gtk_box_append(GTK_BOX(daemon_ctrl_row), self->daemon_toggle_btn);

    gtk_box_append(GTK_BOX(hero_card), daemon_ctrl_row);

    gtk_box_append(GTK_BOX(main_box), hero_card);

    self->lb_active = FALSE;
    self->bonding_active = FALSE;
    update_control_buttons_state(self);

    /* 2. Wi-Fi Adapters Section */
    GtkWidget *dev_sec_lbl = gtk_label_new("WI-FI ADAPTERS");
    gtk_widget_add_css_class(dev_sec_lbl, "section-header");
    gtk_widget_set_halign(dev_sec_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(main_box), dev_sec_lbl);

    self->devices_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(main_box), self->devices_box);

    /* 3. Platform Capabilities Chips Row */
    GtkWidget *caps_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(caps_box, 4);

    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    multiwifi_capabilities_probe(caps);

    GtkWidget *c1 = gtk_label_new("🐧 Linux");
    gtk_widget_add_css_class(c1, "metric-chip");
    gtk_box_append(GTK_BOX(caps_box), c1);

    gboolean nm_ok = (caps->network_manager != MULTIWIFI_CAP_STATE_UNSUPPORTED);
    GtkWidget *c2 = gtk_label_new(nm_ok ? "📶 NetworkManager" : "✕ NetworkManager");
    gtk_widget_add_css_class(c2, "metric-chip");
    gtk_box_append(GTK_BOX(caps_box), c2);

    gboolean mptcp_ok = (caps->kernel_mptcp != MULTIWIFI_CAP_STATE_UNSUPPORTED);
    GtkWidget *c3 = gtk_label_new(mptcp_ok ? "🔀 MPTCP" : "✕ MPTCP");
    gtk_widget_add_css_class(c3, "metric-chip");
    gtk_box_append(GTK_BOX(caps_box), c3);

    gboolean nft_ok = (caps->nftables != MULTIWIFI_CAP_STATE_UNSUPPORTED);
    GtkWidget *c4 = gtk_label_new(nft_ok ? "🛡️ Netfilter" : "✕ Netfilter");
    gtk_widget_add_css_class(c4, "metric-chip");
    gtk_box_append(GTK_BOX(caps_box), c4);

    gboolean tun_ok = (caps->tun_device != MULTIWIFI_CAP_STATE_UNSUPPORTED);
    GtkWidget *c5 = gtk_label_new(tun_ok ? "🔌 TUN" : "✕ TUN");
    gtk_widget_add_css_class(c5, "metric-chip");
    gtk_box_append(GTK_BOX(caps_box), c5);

    multiwifi_capabilities_free(caps);
    gtk_box_append(GTK_BOX(main_box), caps_box);

    /* Initialize Path Monitor */
    self->path_monitor = multiwifi_path_monitor_new();
    g_signal_connect(self->path_monitor, "paths-changed",
                     G_CALLBACK(on_paths_changed), self);
    multiwifi_path_monitor_start(self->path_monitor);

    /* Initialize Inventory */
    self->inventory = multiwifi_inventory_new();
    g_signal_connect(self->inventory, "devices-changed",
                     G_CALLBACK(on_inventory_devices_changed), self);
    multiwifi_inventory_start(self->inventory, NULL);

    update_devices_ui(self);

    /* Footer */
    GtkWidget *footer_lbl = gtk_label_new(NULL);
    gchar *footer_markup = g_strdup_printf("<span size='small' color='#6c7086'>MultiWiFi %s • Unprivileged Session</span>", MULTIWIFI_VERSION_STRING);
    gtk_label_set_markup(GTK_LABEL(footer_lbl), footer_markup);
    g_free(footer_markup);
    gtk_widget_set_halign(footer_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_box), footer_lbl);
}

static void multiwifi_window_class_init(MultiWifiWindowClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_window_dispose;
}

GtkWidget *multiwifi_window_new(GtkApplication *app)
{
    return g_object_new(MULTIWIFI_TYPE_WINDOW, "application", app, NULL);
}
