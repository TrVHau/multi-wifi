#include "window.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/core/path_monitor.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"
#include <gio/gio.h>

/* Embedded CSS Theme Fallback (split into chunks to avoid ISO C overlength warnings) */
static const char CSS_FALLBACK_PART1[] =
"window, .main-window { background-color: #16171f; color: #cdd6f4; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Ubuntu, sans-serif; }\n"
"headerbar { background-color: #1e1e2e; border-bottom: 1px solid rgba(255, 255, 255, 0.08); color: #cdd6f4; padding: 6px 12px; }\n"
"headerbar .title { font-weight: 700; font-size: 14px; color: #cdd6f4; }\n"
"headerbar .subtitle { font-size: 11px; color: #89b4fa; }\n"
".modern-card { background-color: #1e1e2e; border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 14px; padding: 16px 20px; margin-bottom: 4px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.22); }\n"
".hero-card { background: linear-gradient(145deg, #1e1e2e 0%, #26273b 100%); border: 1px solid rgba(137, 180, 250, 0.22); border-radius: 14px; padding: 18px 20px; margin-bottom: 4px; box-shadow: 0 6px 20px rgba(0, 0, 0, 0.28); }\n"
".subtle-card { background-color: #1a1b26; border: 1px solid rgba(255, 255, 255, 0.05); border-radius: 12px; padding: 14px 18px; }\n"
".metrics-card { background-color: rgba(17, 17, 27, 0.6); border: 1px solid rgba(255, 255, 255, 0.06); border-radius: 10px; padding: 10px 14px; }\n"
".section-header { font-size: 11px; font-weight: 800; letter-spacing: 1.2px; color: #89b4fa; text-transform: uppercase; }\n";

static const char CSS_FALLBACK_PART2[] =
".badge { border-radius: 9999px; padding: 3px 12px; font-size: 11px; font-weight: 700; }\n"
".badge-online { background-color: rgba(166, 227, 161, 0.16); color: #a6e3a1; border: 1px solid rgba(166, 227, 161, 0.35); }\n"
".badge-degraded { background-color: rgba(249, 226, 175, 0.16); color: #f9e2af; border: 1px solid rgba(249, 226, 175, 0.35); }\n"
".badge-danger { background-color: rgba(243, 139, 168, 0.16); color: #f38ba8; border: 1px solid rgba(243, 139, 168, 0.35); }\n"
".badge-active { background-color: rgba(137, 180, 250, 0.18); color: #89b4fa; border: 1px solid rgba(137, 180, 250, 0.4); }\n"
".badge-offline { background-color: rgba(108, 112, 134, 0.16); color: #a6adc8; border: 1px solid rgba(108, 112, 134, 0.28); }\n"
".badge-hw { background-color: rgba(148, 226, 213, 0.14); color: #94e2d5; border: 1px solid rgba(148, 226, 213, 0.32); }\n"
".badge-saved { background-color: rgba(203, 166, 247, 0.16); color: #cba6f7; border: 1px solid rgba(203, 166, 247, 0.35); }\n"
"button.btn-primary { background: linear-gradient(135deg, #89b4fa 0%, #3584e4 100%); color: #11111b; font-weight: 700; border-radius: 9px; border: none; padding: 8px 18px; }\n"
"button.btn-primary:hover { background: linear-gradient(135deg, #b4befe 0%, #4a90e2 100%); }\n"
"button.btn-secondary { background-color: rgba(255, 255, 255, 0.06); color: #cdd6f4; border-radius: 9px; border: 1px solid rgba(255, 255, 255, 0.12); font-weight: 600; padding: 8px 16px; }\n"
"button.btn-secondary:hover { background-color: rgba(255, 255, 255, 0.12); border-color: rgba(255, 255, 255, 0.25); }\n"
"button.btn-stop { background-color: rgba(243, 139, 168, 0.15); color: #f38ba8; border-radius: 9px; border: 1px solid rgba(243, 139, 168, 0.35); font-weight: 600; padding: 8px 16px; }\n"
"button.btn-stop:hover { background-color: rgba(243, 139, 168, 0.25); }\n"
"listview, list { background-color: #181825; border-radius: 10px; border: 1px solid rgba(255, 255, 255, 0.08); }\n"
"row { padding: 8px 12px; border-radius: 8px; margin: 3px 6px; }\n"
"row:selected { background-color: rgba(137, 180, 250, 0.25); color: #ffffff; }\n"
"row:hover:not(:selected) { background-color: rgba(255, 255, 255, 0.05); }\n"
"entry, passwordentry { background-color: #181825; color: #cdd6f4; border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 9px; padding: 8px 14px; }\n"
"entry:focus, passwordentry:focus { border-color: #89b4fa; }\n";

struct _MultiWifiWindow {
    GtkApplicationWindow parent_instance;
    GtkWidget *caps_label;
    GtkWidget *daemon_status_lbl;
    GtkWidget *daemon_banner;
    GtkWidget *lb_btn;
    GtkWidget *bonding_btn;
    GtkWidget *devices_box;
    GtkWidget *mode_pill;
    GtkWidget *mode_title_lbl;
    GtkWidget *mode_desc_lbl;
    MultiWifiInventory *inventory;
    MultiWifiPathMonitor *path_monitor;
    guint bus_watcher_id;
    GDBusConnection *system_bus;
    gboolean daemon_connected;
    gboolean lb_active;
    gboolean bonding_active;
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
static void on_lb_clicked(GtkButton *btn, gpointer user_data);
static void on_bonding_clicked(GtkButton *btn, gpointer user_data);
static void on_diagnostics_clicked(GtkButton *btn, gpointer user_data);
static void on_select_wifi_clicked(GtkButton *btn, gpointer user_data);
static void on_inventory_devices_changed(MultiWifiInventory *inv, gpointer user_data);
static void on_paths_changed(MultiWifiPathMonitor *mon, gpointer user_data);
static void on_status_query_done(GObject *source, GAsyncResult *res, gpointer user_data);

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
        gtk_button_set_label(GTK_BUTTON(dlg->connect_btn), "Connect (Saved)");
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
    /* Clear existing list box rows */
    GtkWidget *child = gtk_widget_get_first_child(dlg->list_box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(dlg->list_box), child);
        child = next;
    }

    GPtrArray *aps = multiwifi_inventory_dup_access_points(dlg->main_win->inventory, dlg->ifname);
    if (aps == NULL || aps->len == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new("No access points discovered. Click 'Rescan' to scan for nearby networks.");
        gtk_widget_set_margin_top(lbl, 18);
        gtk_widget_set_margin_bottom(lbl, 18);
        gtk_widget_set_margin_start(lbl, 14);
        gtk_widget_set_margin_end(lbl, 14);
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
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_top(hbox, 8);
        gtk_widget_set_margin_bottom(hbox, 8);
        gtk_widget_set_margin_start(hbox, 12);
        gtk_widget_set_margin_end(hbox, 12);

        /* Signal strength bar indicator */
        const gchar *bars = (ap->signal_strength >= 75) ? "▂▄▆█" :
                            (ap->signal_strength >= 50) ? "▂▄▆_" :
                            (ap->signal_strength >= 25) ? "▂▄__" : "▂___";

        /* Signal icon & percentage */
        GtkWidget *sig_lbl = gtk_label_new(NULL);
        gchar *sig_markup = g_strdup_printf(
            "<span font_family='monospace' size='medium' weight='bold' color='%s'>%s</span> <span size='small' color='#a6adc8'>%u%%</span>",
            (ap->signal_strength >= 60) ? "#a6e3a1" : (ap->signal_strength >= 35) ? "#f9e2af" : "#f38ba8",
            bars, ap->signal_strength
        );
        gtk_label_set_markup(GTK_LABEL(sig_lbl), sig_markup);
        gtk_box_append(GTK_BOX(hbox), sig_lbl);
        g_free(sig_markup);

        /* SSID and Frequency label */
        gchar *escaped_ssid = g_markup_escape_text(ap->ssid ? ap->ssid : "Hidden", -1);
        double freq_ghz = (double)ap->frequency / 1000.0;
        gchar *name_markup = g_strdup_printf(
            "<span weight='bold' size='medium'>%s</span>  <span size='small' color='#89b4fa'>%.1f GHz</span>",
            escaped_ssid, freq_ghz
        );
        GtkWidget *name_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(name_lbl), name_markup);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(name_lbl, TRUE);
        gtk_box_append(GTK_BOX(hbox), name_lbl);
        g_free(name_markup);
        g_free(escaped_ssid);

        /* Badges: Connected, Saved, Security */
        if (ap->is_active) {
            GtkWidget *act_lbl = gtk_label_new("● Connected");
            gtk_widget_add_css_class(act_lbl, "badge");
            gtk_widget_add_css_class(act_lbl, "badge-online");
            gtk_box_append(GTK_BOX(hbox), act_lbl);
        } else if (ap->is_saved) {
            GtkWidget *sav_lbl = gtk_label_new("Saved");
            gtk_widget_add_css_class(sav_lbl, "badge");
            gtk_widget_add_css_class(sav_lbl, "badge-saved");
            gtk_box_append(GTK_BOX(hbox), sav_lbl);
        }

        /* Security badge */
        GtkWidget *sec_lbl = gtk_label_new(NULL);
        gboolean is_open = (g_strcmp0(ap->security_str, "Open") == 0);
        gchar *sec_markup = g_strdup_printf("<span size='small' color='#a6adc8'>%s %s</span>",
            is_open ? "🔓" : "🔒",
            ap->security_str ? ap->security_str : "Open");
        gtk_label_set_markup(GTK_LABEL(sec_lbl), sec_markup);
        gtk_box_append(GTK_BOX(hbox), sec_lbl);
        g_free(sec_markup);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

        g_object_set_data_full(G_OBJECT(row), "ssid", g_strdup(ap->ssid), g_free);
        g_object_set_data(G_OBJECT(row), "is_saved", GINT_TO_POINTER(ap->is_saved));
        g_object_set_data(G_OBJECT(row), "is_open", GINT_TO_POINTER(is_open));

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
    gtk_label_set_markup(GTK_LABEL(dlg->status_lbl),
        "<span size='small' color='#a6e3a1'>Scan complete. Select a network to connect.</span>");
}

static void on_dialog_scan_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    gtk_widget_set_sensitive(dlg->scan_btn, FALSE);
    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    gtk_spinner_start(GTK_SPINNER(dlg->spinner));
    gtk_label_set_markup(GTK_LABEL(dlg->status_lbl),
        "<span size='small' color='#89b4fa'>Scanning for nearby Wi-Fi networks...</span>");

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
        gtk_label_set_markup(GTK_LABEL(dlg->status_lbl),
            "<span size='small' color='#a6e3a1'><b>Connected successfully!</b></span>");
        gtk_window_destroy(GTK_WINDOW(dlg->dialog_window));
    } else {
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
        gtk_widget_set_sensitive(dlg->scan_btn, TRUE);
        gtk_widget_set_sensitive(dlg->list_box, TRUE);

        gchar *escaped_msg = g_markup_escape_text(error ? error->message : "Unknown error", -1);
        gchar *err_markup = g_strdup_printf("<span size='small' color='#f38ba8'><b>Connection failed:</b> %s</span>", escaped_msg);
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
        "<span size='small' color='#89b4fa'>Connecting %s to '%s'...</span>",
        dlg->ifname, escaped_ssid
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

    gchar *win_title = g_strdup_printf("Select Wi-Fi Network (%s)", ifname);
    gtk_window_set_title(GTK_WINDOW(dlg->dialog_window), win_title);
    g_free(win_title);

    gtk_window_set_default_size(GTK_WINDOW(dlg->dialog_window), 540, 520);
    gtk_window_set_transient_for(GTK_WINDOW(dlg->dialog_window), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(dlg->dialog_window), TRUE);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dlg->dialog_window), header);

    dlg->spinner = gtk_spinner_new();
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), dlg->spinner);

    dlg->scan_btn = gtk_button_new_with_label("Rescan");
    gtk_widget_add_css_class(dlg->scan_btn, "btn-secondary");
    gtk_widget_set_tooltip_text(dlg->scan_btn, "Scan for nearby Wi-Fi access points");
    g_signal_connect(dlg->scan_btn, "clicked", G_CALLBACK(on_dialog_scan_clicked), dlg);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), dlg->scan_btn);

    /* Main Container */
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(content_box, 16);
    gtk_widget_set_margin_bottom(content_box, 16);
    gtk_widget_set_margin_start(content_box, 18);
    gtk_widget_set_margin_end(content_box, 18);

    /* Subtitle */
    gchar *sub_markup = g_strdup_printf(
        "<span size='small' color='#a6adc8'>Select a Wi-Fi network to connect adapter <b color='#89b4fa'>%s</b>:</span>",
        ifname
    );
    GtkWidget *sub_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sub_lbl), sub_markup);
    gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content_box), sub_lbl);
    g_free(sub_markup);

    /* List Box in Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);

    dlg->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dlg->list_box), GTK_SELECTION_SINGLE);
    g_signal_connect(dlg->list_box, "row-selected", G_CALLBACK(on_wifi_row_selected), dlg);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dlg->list_box);
    gtk_box_append(GTK_BOX(content_box), scrolled);

    /* Password Input Box (Hidden initially) */
    dlg->password_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_visible(dlg->password_box, FALSE);

    GtkWidget *pwd_lbl = gtk_label_new("Password:");
    gtk_box_append(GTK_BOX(dlg->password_box), pwd_lbl);

    dlg->password_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(dlg->password_entry), TRUE);
    gtk_widget_set_hexpand(dlg->password_entry, TRUE);
    gtk_box_append(GTK_BOX(dlg->password_box), dlg->password_entry);
    gtk_box_append(GTK_BOX(content_box), dlg->password_box);

    /* Status Label */
    dlg->status_lbl = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(dlg->status_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dlg->status_lbl), 0.0f);
    gtk_box_append(GTK_BOX(content_box), dlg->status_lbl);

    /* Bottom Button Row */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel_btn, "btn-secondary");
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_destroy), dlg->dialog_window);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    dlg->connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(dlg->connect_btn, "btn-primary");
    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    g_signal_connect(dlg->connect_btn, "clicked", G_CALLBACK(on_dialog_connect_clicked), dlg);
    gtk_box_append(GTK_BOX(btn_row), dlg->connect_btn);

    gtk_box_append(GTK_BOX(content_box), btn_row);

    gtk_window_set_child(GTK_WINDOW(dlg->dialog_window), content_box);

    /* Cleanup when window is destroyed */
    g_signal_connect_swapped(dlg->dialog_window, "destroy", G_CALLBACK(wifi_select_dialog_free), dlg);

    /* Initial populate */
    populate_wifi_list(dlg);
    gtk_window_present(GTK_WINDOW(dlg->dialog_window));
}

static void update_devices_ui(MultiWifiWindow *self)
{
    if (self->devices_box == NULL || self->inventory == NULL) {
        return;
    }

    /* Clear previous device widgets from container */
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

        GtkWidget *empty_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(empty_label),
            "<span color='#a6adc8'><i>Scanning for physical Wi-Fi adapters or NetworkManager not ready...</i></span>");
        gtk_widget_set_halign(empty_label, GTK_ALIGN_START);
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

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_add_css_class(card_box, "modern-card");

        /* Header row: Hardware icon, Name, MAC, Type Badge, Status Badge */
        GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

        gboolean is_usb = (info->ifname && strstr(info->ifname, "u") != NULL);
        const gchar *hw_icon = is_usb ? "🔌" : "💻";
        const gchar *type_desc = is_usb ? "USB Adapter" : "PCIe / Internal";

        gchar *title_markup = g_strdup_printf(
            "<span size='medium' weight='bold'>%s %s</span>  <span size='small' color='#6c7086'>[%s]</span>",
            hw_icon,
            info->ifname ? info->ifname : "unknown",
            info->hw_address ? info->hw_address : "no-mac"
        );
        GtkWidget *title_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(title_lbl), title_markup);
        gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(header_row), title_lbl);
        g_free(title_markup);

        /* Hardware type badge */
        GtkWidget *hw_badge = gtk_label_new(type_desc);
        gtk_widget_add_css_class(hw_badge, "badge");
        gtk_widget_add_css_class(hw_badge, "badge-hw");
        gtk_box_append(GTK_BOX(header_row), hw_badge);

        /* Spacer to push status badge to the right */
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(header_row), spacer);

        /* Status badge */
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
            gtk_label_set_text(GTK_LABEL(badge_lbl), "○ Disconnected");
            gtk_widget_add_css_class(badge_lbl, "badge-offline");
        }
        gtk_box_append(GTK_BOX(header_row), badge_lbl);
        gtk_box_append(GTK_BOX(card_box), header_row);

        /* Connection / SSID Details */
        if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            GtkWidget *ssid_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            double freq_ghz = (double)info->frequency / 1000.0;
            gchar *escaped_ssid = g_markup_escape_text(info->ssid ? info->ssid : "Hidden", -1);

            const gchar *bars = (info->signal_strength >= 75) ? "▂▄▆█" :
                                (info->signal_strength >= 50) ? "▂▄▆_" :
                                (info->signal_strength >= 25) ? "▂▄__" : "▂___";

            gchar *ssid_markup = g_strdup_printf(
                "<span size='x-large' weight='bold'>📶 %s</span>", escaped_ssid
            );
            GtkWidget *ssid_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(ssid_lbl), ssid_markup);
            gtk_box_append(GTK_BOX(ssid_box), ssid_lbl);
            g_free(ssid_markup);

            /* Frequency badge */
            gchar *freq_str = g_strdup_printf("%.1f GHz", freq_ghz);
            GtkWidget *freq_badge = gtk_label_new(freq_str);
            gtk_widget_add_css_class(freq_badge, "badge");
            gtk_widget_add_css_class(freq_badge, "badge-active");
            gtk_box_append(GTK_BOX(ssid_box), freq_badge);
            g_free(freq_str);

            /* Signal meter badge */
            gchar *sig_markup = g_strdup_printf(
                "<span font_family='monospace' size='medium' weight='bold' color='%s'>%s %u%%</span>",
                (info->signal_strength >= 60) ? "#a6e3a1" : (info->signal_strength >= 35) ? "#f9e2af" : "#f38ba8",
                bars, info->signal_strength
            );
            GtkWidget *sig_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(sig_lbl), sig_markup);
            gtk_box_append(GTK_BOX(ssid_box), sig_lbl);
            g_free(sig_markup);

            gtk_box_append(GTK_BOX(card_box), ssid_box);

            /* IP & Profile Details */
            gchar *escaped_profile = g_markup_escape_text(info->connection_id ? info->connection_id : "Active", -1);
            gchar *details_markup = g_strdup_printf(
                "<span size='small' color='#a6adc8'><b>IPv4:</b> <tt>%s</tt>    <b>Gateway:</b> <tt>%s</tt>    <b>Profile:</b> %s</span>",
                info->ipv4_address ? info->ipv4_address : "Pending DHCP",
                info->ipv4_gateway ? info->ipv4_gateway : "None",
                escaped_profile
            );
            GtkWidget *details_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(details_lbl), details_markup);
            gtk_label_set_xalign(GTK_LABEL(details_lbl), 0.0f);
            gtk_box_append(GTK_BOX(card_box), details_lbl);
            g_free(details_markup);
            g_free(escaped_profile);
            g_free(escaped_ssid);
        } else {
            GtkWidget *idle_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(idle_lbl),
                "<span size='small' color='#6c7086'><i>Adapter is idle. Ready to connect to Wi-Fi.</i></span>");
            gtk_label_set_xalign(GTK_LABEL(idle_lbl), 0.0f);
            gtk_box_append(GTK_BOX(card_box), idle_lbl);
        }

        /* Path Diagnostics Metrics Card */
        if (path != NULL && path->state != MULTIWIFI_PATH_STATE_OFFLINE) {
            GtkWidget *metrics_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_add_css_class(metrics_box, "metrics-card");

            const gchar *eligibility_str = path->is_eligible
                ? "<span color='#a6e3a1' weight='bold'>Eligible</span>"
                : "<span color='#f38ba8' weight='bold'>Excluded</span>";

            const gchar *loss_color = (path->packet_loss_percent == 0) ? "#a6e3a1" :
                                      (path->packet_loss_percent <= 10) ? "#f9e2af" : "#f38ba8";

            gchar *escaped_reason = g_markup_escape_text(path->status_reason ? path->status_reason : "Healthy", -1);
            gchar *metrics_markup = g_strdup_printf(
                "<span size='small' color='#cdd6f4'>"
                "<b>Path Quality:</b> Latency: <span weight='bold' color='#89b4fa'>%.1f ms</span> (min: %.1f, max: %.1f, jitter: %.1f)  •  "
                "Loss: <span weight='bold' color='%s'>%u%%</span>  •  "
                "Status: <b>%s</b>  •  Multi-Path: %s</span>",
                path->rtt_ms,
                path->min_rtt_ms,
                path->max_rtt_ms,
                path->jitter_ms,
                loss_color,
                path->packet_loss_percent,
                escaped_reason,
                eligibility_str
            );
            GtkWidget *metrics_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(metrics_lbl), metrics_markup);
            gtk_label_set_xalign(GTK_LABEL(metrics_lbl), 0.0f);
            gtk_box_append(GTK_BOX(metrics_box), metrics_lbl);
            g_free(metrics_markup);
            g_free(escaped_reason);

            gtk_box_append(GTK_BOX(card_box), metrics_box);
        }

        /* Action row: Wi-Fi network selection button */
        GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(action_row, 4);

        const gchar *btn_label = (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED)
            ? "📶 Switch Wi-Fi Network..." : "📶 Connect to Wi-Fi...";
        GtkWidget *wifi_btn = gtk_button_new_with_label(btn_label);
        gtk_widget_add_css_class(wifi_btn, "btn-secondary");
        gtk_widget_set_tooltip_text(wifi_btn, "Scan and associate this adapter with a distinct Wi-Fi network");
        g_object_set_data_full(G_OBJECT(wifi_btn), "ifname", g_strdup(info->ifname), g_free);
        g_signal_connect(wifi_btn, "clicked", G_CALLBACK(on_select_wifi_clicked), self);
        gtk_box_append(GTK_BOX(action_row), wifi_btn);
        gtk_box_append(GTK_BOX(card_box), action_row);

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
    if (self->lb_btn == NULL || self->bonding_btn == NULL || self->daemon_banner == NULL) {
        return;
    }

    if (self->daemon_connected) {
        if (self->daemon_status_lbl != NULL) {
            gtk_widget_remove_css_class(self->daemon_status_lbl, "badge-offline");
            gtk_widget_add_css_class(self->daemon_status_lbl, "badge-online");
            gtk_label_set_text(GTK_LABEL(self->daemon_status_lbl), "● Daemon Active");
        }

        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#a6e3a1'>✔ <b>System Daemon Active:</b> Connected via System D-Bus. Privileged actions authorized via Polkit.</span>");

        gtk_widget_set_sensitive(self->lb_btn, !self->bonding_active);
        gtk_widget_set_sensitive(self->bonding_btn, !self->lb_active);

        /* LB Button State & Styling */
        if (self->lb_active) {
            gtk_button_set_label(GTK_BUTTON(self->lb_btn), "⏹ Stop Load Balancing");
            gtk_widget_remove_css_class(self->lb_btn, "btn-primary");
            gtk_widget_add_css_class(self->lb_btn, "btn-stop");
            gtk_widget_set_tooltip_text(self->lb_btn, "Stop active multi-path policy routing and restore default route");
        } else {
            gtk_button_set_label(GTK_BUTTON(self->lb_btn), "⚡ Start Load Balancing");
            gtk_widget_remove_css_class(self->lb_btn, "btn-stop");
            gtk_widget_add_css_class(self->lb_btn, "btn-primary");
            gtk_widget_set_tooltip_text(self->lb_btn, "Aggregate connected Wi-Fi uplinks with flow-affinity load balancing");
        }

        /* Bonding Button State & Styling */
        if (self->bonding_active) {
            gtk_button_set_label(GTK_BUTTON(self->bonding_btn), "⏹ Stop Bonding");
            gtk_widget_remove_css_class(self->bonding_btn, "btn-secondary");
            gtk_widget_add_css_class(self->bonding_btn, "btn-stop");
            gtk_widget_set_tooltip_text(self->bonding_btn, "Terminate MPTCP bonding tunnel");
        } else {
            gtk_button_set_label(GTK_BUTTON(self->bonding_btn), "🔗 Start Bonding");
            gtk_widget_remove_css_class(self->bonding_btn, "btn-stop");
            gtk_widget_add_css_class(self->bonding_btn, "btn-secondary");
            gtk_widget_set_tooltip_text(self->bonding_btn, "Establish MPTCP bonding tunnel through aggregation server");
        }

        /* Mode Banner info */
        if (self->mode_pill != NULL && self->mode_title_lbl != NULL && self->mode_desc_lbl != NULL) {
            if (self->bonding_active) {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "MPTCP Bonding");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_remove_css_class(self->mode_pill, "badge-online");
                gtk_widget_add_css_class(self->mode_pill, "badge-active");
                gtk_label_set_markup(GTK_LABEL(self->mode_title_lbl),
                    "<span size='medium' weight='bold' color='#cdd6f4'>Multi-Path Tunnel Bonding</span>");
                gtk_label_set_markup(GTK_LABEL(self->mode_desc_lbl),
                    "<span size='small' color='#a6adc8'>Single TCP stream aggregation over multiple uplinks with out-of-order reassembly.</span>");
            } else if (self->lb_active) {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "Round-Robin LB");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_remove_css_class(self->mode_pill, "badge-online");
                gtk_widget_add_css_class(self->mode_pill, "badge-active");
                gtk_label_set_markup(GTK_LABEL(self->mode_title_lbl),
                    "<span size='medium' weight='bold' color='#cdd6f4'>Flow Multiplexing Load Balancer</span>");
                gtk_label_set_markup(GTK_LABEL(self->mode_desc_lbl),
                    "<span size='small' color='#a6adc8'>Per-flow hash steering balancing sockets and connections across active Wi-Fi adapters.</span>");
            } else {
                gtk_label_set_text(GTK_LABEL(self->mode_pill), "Active-Backup");
                gtk_widget_remove_css_class(self->mode_pill, "badge-active");
                gtk_widget_remove_css_class(self->mode_pill, "badge-offline");
                gtk_widget_add_css_class(self->mode_pill, "badge-online");
                gtk_label_set_markup(GTK_LABEL(self->mode_title_lbl),
                    "<span size='medium' weight='bold' color='#cdd6f4'>Dual-Link Multi-Homing Ready</span>");
                gtk_label_set_markup(GTK_LABEL(self->mode_desc_lbl),
                    "<span size='small' color='#a6adc8'>Multiple interfaces connected and monitored with automatic health-aware failover.</span>");
            }
        }
    } else {
        if (self->daemon_status_lbl != NULL) {
            gtk_widget_remove_css_class(self->daemon_status_lbl, "badge-online");
            gtk_widget_add_css_class(self->daemon_status_lbl, "badge-offline");
            gtk_label_set_text(GTK_LABEL(self->daemon_status_lbl), "○ Standalone");
        }

        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#f38ba8'>✕ <b>System Daemon Inactive:</b> Privileged network actions require <tt>multiwifi-daemon</tt>.\n"
            "Run <tt>sudo systemctl start multiwifi-daemon</tt> to enable Load Balancing &amp; Bonding controls.</span>");

        gtk_widget_set_sensitive(self->lb_btn, FALSE);
        gtk_widget_set_sensitive(self->bonding_btn, FALSE);
        gtk_widget_set_tooltip_text(self->lb_btn, "Unavailable: MultiWiFi daemon is not running on system bus");
        gtk_widget_set_tooltip_text(self->bonding_btn, "Unavailable: MultiWiFi daemon is not running on system bus");

        if (self->mode_pill != NULL && self->mode_title_lbl != NULL && self->mode_desc_lbl != NULL) {
            gtk_label_set_text(GTK_LABEL(self->mode_pill), "Standalone");
            gtk_widget_remove_css_class(self->mode_pill, "badge-active");
            gtk_widget_remove_css_class(self->mode_pill, "badge-online");
            gtk_widget_add_css_class(self->mode_pill, "badge-offline");
            gtk_label_set_markup(GTK_LABEL(self->mode_title_lbl),
                "<span size='medium' weight='bold' color='#cdd6f4'>Monitoring &amp; Inventory Mode</span>");
            gtk_label_set_markup(GTK_LABEL(self->mode_desc_lbl),
                "<span size='small' color='#a6adc8'>Monitoring adapter quality and network availability. Daemon required for routing mutation.</span>");
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

static void on_lb_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (!self->daemon_connected || self->system_bus == NULL) {
        return;
    }

    if (self->lb_active) {
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
    } else {
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
    }
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

static void on_bonding_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    if (!self->daemon_connected || self->system_bus == NULL) {
        return;
    }

    if (self->bonding_active) {
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
    } else {
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
    }
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
    if (self->system_bus == NULL) {
        self->system_bus = g_object_ref(connection);
    }

    /* Query current mode on daemon */
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

static void multiwifi_window_dispose(GObject *object)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(object);

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
    /* Apply modern CSS stylesheet */
    multiwifi_window_apply_css();

    gtk_window_set_title(GTK_WINDOW(self), "MultiWiFi");
    gtk_window_set_default_size(GTK_WINDOW(self), 760, 680);
    gtk_widget_add_css_class(GTK_WIDGET(self), "main-window");

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(self), header);

    /* Center Title Widget */
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);

    GtkWidget *title_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_lbl), "<span weight='bold' size='medium'>MultiWiFi</span>");
    gtk_box_append(GTK_BOX(title_box), title_lbl);

    GtkWidget *sub_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sub_lbl), "<span size='small' color='#89b4fa'>Multi-Path Wi-Fi Bonding &amp; Load Balancer</span>");
    gtk_box_append(GTK_BOX(title_box), sub_lbl);

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_box);

    /* Left: Daemon Status Badge */
    self->daemon_status_lbl = gtk_label_new("○ Standalone");
    gtk_widget_add_css_class(self->daemon_status_lbl, "badge");
    gtk_widget_add_css_class(self->daemon_status_lbl, "badge-offline");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), self->daemon_status_lbl);

    /* Right: Probe Paths Button */
    GtkWidget *diag_btn = gtk_button_new_with_label("↻ Probe Paths");
    gtk_widget_add_css_class(diag_btn, "btn-secondary");
    gtk_widget_set_tooltip_text(diag_btn, "Trigger real-time latency & quality diagnostics cycle across all uplinks");
    g_signal_connect(diag_btn, "clicked", G_CALLBACK(on_diagnostics_clicked), self);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), diag_btn);

    /* Watch system bus for MultiWiFi System Daemon */
    self->system_bus = NULL;
    self->daemon_connected = FALSE;
    self->bus_watcher_id = g_bus_watch_name(
        G_BUS_TYPE_SYSTEM,
        MULTIWIFI_DBUS_NAME,
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_daemon_name_appeared,
        on_daemon_name_vanished,
        self,
        NULL
    );

    /* Scrolled Window Container */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    /* Main Box inside Scrolled Window */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(main_box, 20);
    gtk_widget_set_margin_bottom(main_box, 20);
    gtk_widget_set_margin_start(main_box, 20);
    gtk_widget_set_margin_end(main_box, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), main_box);
    gtk_window_set_child(GTK_WINDOW(self), scrolled);

    /* 1. Hero Dashboard Card: Multi-Path Operations */
    GtkWidget *hero_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(hero_card, "hero-card");

    GtkWidget *hero_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *hero_sec_lbl = gtk_label_new("MULTI-PATH TRAFFIC CONTROLLER");
    gtk_widget_add_css_class(hero_sec_lbl, "section-header");
    gtk_widget_set_halign(hero_sec_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(hero_sec_lbl, TRUE);
    gtk_box_append(GTK_BOX(hero_top), hero_sec_lbl);

    self->mode_pill = gtk_label_new("Active-Backup");
    gtk_widget_add_css_class(self->mode_pill, "badge");
    gtk_widget_add_css_class(self->mode_pill, "badge-online");
    gtk_box_append(GTK_BOX(hero_top), self->mode_pill);
    gtk_box_append(GTK_BOX(hero_card), hero_top);

    self->mode_title_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(self->mode_title_lbl),
        "<span size='medium' weight='bold' color='#cdd6f4'>Dual-Link Multi-Homing Ready</span>");
    gtk_label_set_xalign(GTK_LABEL(self->mode_title_lbl), 0.0f);
    gtk_box_append(GTK_BOX(hero_card), self->mode_title_lbl);

    self->mode_desc_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(self->mode_desc_lbl),
        "<span size='small' color='#a6adc8'>Multiple interfaces connected and monitored with automatic health-aware failover.</span>");
    gtk_label_set_xalign(GTK_LABEL(self->mode_desc_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(self->mode_desc_lbl), TRUE);
    gtk_box_append(GTK_BOX(hero_card), self->mode_desc_lbl);

    /* Action buttons row */
    GtkWidget *ops_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(ops_box, 6);

    self->lb_btn = gtk_button_new_with_label("⚡ Start Load Balancing");
    gtk_widget_add_css_class(self->lb_btn, "btn-primary");
    g_signal_connect(self->lb_btn, "clicked", G_CALLBACK(on_lb_clicked), self);
    gtk_box_append(GTK_BOX(ops_box), self->lb_btn);

    self->bonding_btn = gtk_button_new_with_label("🔗 Start Bonding");
    gtk_widget_add_css_class(self->bonding_btn, "btn-secondary");
    g_signal_connect(self->bonding_btn, "clicked", G_CALLBACK(on_bonding_clicked), self);
    gtk_box_append(GTK_BOX(ops_box), self->bonding_btn);

    gtk_box_append(GTK_BOX(hero_card), ops_box);

    /* Daemon status banner inside hero card */
    self->daemon_banner = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(self->daemon_banner), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->daemon_banner), 0.0f);
    gtk_box_append(GTK_BOX(hero_card), self->daemon_banner);

    gtk_box_append(GTK_BOX(main_box), hero_card);

    self->lb_active = FALSE;
    self->bonding_active = FALSE;
    update_control_buttons_state(self);

    /* 2. Discovered Wi-Fi Uplinks Section */
    GtkWidget *dev_sec_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *dev_sec_title = gtk_label_new("PHYSICAL WI-FI ADAPTERS & UPLINKS");
    gtk_widget_add_css_class(dev_sec_title, "section-header");
    gtk_widget_set_halign(dev_sec_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(dev_sec_box), dev_sec_title);

    GtkWidget *dev_sec_sub = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dev_sec_sub),
        "<span size='small' color='#a6adc8'>Assign distinct Wi-Fi networks to each physical adapter to aggregate bandwidth.</span>");
    gtk_widget_set_halign(dev_sec_sub, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(dev_sec_box), dev_sec_sub);
    gtk_box_append(GTK_BOX(main_box), dev_sec_box);

    self->devices_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(main_box), self->devices_box);

    /* 3. Platform & Kernel Engine Capabilities Card */
    GtkWidget *caps_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(caps_card, "subtle-card");

    GtkWidget *caps_title = gtk_label_new("SYSTEM ENGINE & PLATFORM STATUS");
    gtk_widget_add_css_class(caps_title, "section-header");
    gtk_widget_set_halign(caps_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(caps_card), caps_title);

    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    multiwifi_capabilities_probe(caps);

    gchar *escaped_mptcp = g_markup_escape_text(caps->mptcp_details ? caps->mptcp_details : "none", -1);
    gchar *caps_text = g_strdup_printf(
        "<span size='small' color='#cdd6f4'>"
        "✔ <b>Kernel:</b> %s    •    "
        "✔ <b>NetworkManager:</b> %s    •    "
        "✔ <b>MPTCP:</b> %s (%s)\n"
        "✔ <b>nftables / Netfilter:</b> %s    •    "
        "✔ <b>TUN Interface:</b> %s</span>",
        caps->kernel_version ? caps->kernel_version : "Unknown",
        multiwifi_cap_state_to_string(caps->network_manager),
        multiwifi_cap_state_to_string(caps->kernel_mptcp),
        escaped_mptcp,
        multiwifi_cap_state_to_string(caps->nftables),
        multiwifi_cap_state_to_string(caps->tun_device)
    );
    g_free(escaped_mptcp);

    self->caps_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(self->caps_label), caps_text);
    gtk_label_set_xalign(GTK_LABEL(self->caps_label), 0.0f);
    gtk_box_append(GTK_BOX(caps_card), self->caps_label);

    g_free(caps_text);
    multiwifi_capabilities_free(caps);

    gtk_box_append(GTK_BOX(main_box), caps_card);

    /* Initialize Path Monitor (Phase 2) */
    self->path_monitor = multiwifi_path_monitor_new();
    g_signal_connect(self->path_monitor, "paths-changed",
                     G_CALLBACK(on_paths_changed), self);
    multiwifi_path_monitor_start(self->path_monitor);

    /* Initialize Inventory (Phase 1) */
    self->inventory = multiwifi_inventory_new();
    g_signal_connect(self->inventory, "devices-changed",
                     G_CALLBACK(on_inventory_devices_changed), self);
    multiwifi_inventory_start(self->inventory, NULL);

    /* Initial UI render */
    update_devices_ui(self);

    /* Footer */
    GtkWidget *footer_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_lbl),
        "<span size='small' color='#6c7086'>MultiWiFi " MULTIWIFI_VERSION_STRING " • Native C11 Desktop Engine • Unprivileged User Session</span>");
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
