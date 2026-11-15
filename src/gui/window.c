#include "window.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/core/path_monitor.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"
#include <gio/gio.h>

struct _MultiWifiWindow {
    GtkApplicationWindow parent_instance;
    GtkWidget *caps_label;
    GtkWidget *daemon_status_lbl;
    GtkWidget *daemon_banner;
    GtkWidget *lb_btn;
    GtkWidget *bonding_btn;
    GtkWidget *devices_box;
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
        GtkWidget *lbl = gtk_label_new("No access points discovered. Click 'Rescan' to scan for networks.");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_set_margin_bottom(lbl, 16);
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_end(lbl, 12);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_append(GTK_LIST_BOX(dlg->list_box), row);
        if (aps != NULL) g_ptr_array_unref(aps);
        return;
    }

    for (guint i = 0; i < aps->len; i++) {
        MultiWifiAccessPointInfo *ap = g_ptr_array_index(aps, i);

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(hbox, 8);
        gtk_widget_set_margin_bottom(hbox, 8);
        gtk_widget_set_margin_start(hbox, 12);
        gtk_widget_set_margin_end(hbox, 12);

        /* Signal strength bar indicator */
        const gchar *bars = (ap->signal_strength >= 75) ? "▂▄▆█" :
                            (ap->signal_strength >= 50) ? "▂▄▆_" :
                            (ap->signal_strength >= 25) ? "▂▄__" : "▂___";

        /* SSID and Frequency label */
        gchar *escaped_ssid = g_markup_escape_text(ap->ssid ? ap->ssid : "Hidden", -1);
        double freq_ghz = (double)ap->frequency / 1000.0;
        gchar *name_markup = g_strdup_printf(
            "<span weight='bold'>%s</span>  <span size='small' color='#888'>%.1f GHz</span>",
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
            GtkWidget *act_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(act_lbl), "<span foreground='#4caf50' weight='bold' size='small'>● Connected</span>");
            gtk_box_append(GTK_BOX(hbox), act_lbl);
        } else if (ap->is_saved) {
            GtkWidget *sav_lbl = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(sav_lbl), "<span foreground='#2196f3' size='small'>[Saved]</span>");
            gtk_box_append(GTK_BOX(hbox), sav_lbl);
        }

        /* Security string */
        GtkWidget *sec_lbl = gtk_label_new(NULL);
        gchar *sec_markup = g_strdup_printf("<span size='small' color='#aaa'>%s</span>",
            ap->security_str ? ap->security_str : "Open");
        gtk_label_set_markup(GTK_LABEL(sec_lbl), sec_markup);
        gtk_box_append(GTK_BOX(hbox), sec_lbl);
        g_free(sec_markup);

        /* Signal percentage */
        GtkWidget *sig_lbl = gtk_label_new(NULL);
        gchar *sig_markup = g_strdup_printf("<span font_family='monospace' size='small'>%s %u%%</span>", bars, ap->signal_strength);
        gtk_label_set_markup(GTK_LABEL(sig_lbl), sig_markup);
        gtk_box_append(GTK_BOX(hbox), sig_lbl);
        g_free(sig_markup);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

        gboolean is_open = (g_strcmp0(ap->security_str, "Open") == 0);
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
        "<span size='small' color='#388e3c'>Scan complete. Select a network to connect.</span>");
}

static void on_dialog_scan_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    WifiSelectDialog *dlg = (WifiSelectDialog *)user_data;
    gtk_widget_set_sensitive(dlg->scan_btn, FALSE);
    gtk_widget_set_sensitive(dlg->connect_btn, FALSE);
    gtk_spinner_start(GTK_SPINNER(dlg->spinner));
    gtk_label_set_markup(GTK_LABEL(dlg->status_lbl),
        "<span size='small' color='#2196f3'>Scanning for Wi-Fi networks...</span>");

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
            "<span size='small' color='#388e3c'><b>Connected successfully!</b></span>");
        gtk_window_destroy(GTK_WINDOW(dlg->dialog_window));
    } else {
        gtk_widget_set_sensitive(dlg->connect_btn, TRUE);
        gtk_widget_set_sensitive(dlg->scan_btn, TRUE);
        gtk_widget_set_sensitive(dlg->list_box, TRUE);

        gchar *escaped_msg = g_markup_escape_text(error ? error->message : "Unknown error", -1);
        gchar *err_markup = g_strdup_printf("<span size='small' color='#d32f2f'><b>Connection failed:</b> %s</span>", escaped_msg);
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
        "<span size='small' color='#2196f3'>Connecting %s to '%s'...</span>",
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
    gchar *win_title = g_strdup_printf("Select Wi-Fi Network (%s)", ifname);
    gtk_window_set_title(GTK_WINDOW(dlg->dialog_window), win_title);
    g_free(win_title);

    gtk_window_set_default_size(GTK_WINDOW(dlg->dialog_window), 480, 500);
    gtk_window_set_transient_for(GTK_WINDOW(dlg->dialog_window), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(dlg->dialog_window), TRUE);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dlg->dialog_window), header);

    dlg->spinner = gtk_spinner_new();
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), dlg->spinner);

    dlg->scan_btn = gtk_button_new_with_label("Rescan");
    gtk_widget_set_tooltip_text(dlg->scan_btn, "Scan for nearby Wi-Fi access points");
    g_signal_connect(dlg->scan_btn, "clicked", G_CALLBACK(on_dialog_scan_clicked), dlg);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), dlg->scan_btn);

    /* Main Container */
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content_box, 14);
    gtk_widget_set_margin_bottom(content_box, 14);
    gtk_widget_set_margin_start(content_box, 16);
    gtk_widget_set_margin_end(content_box, 16);

    /* Subtitle */
    gchar *sub_markup = g_strdup_printf(
        "<span size='small' color='#666'>Select a Wi-Fi network to connect adapter <b>%s</b>:</span>",
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
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), TRUE);

    dlg->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dlg->list_box), GTK_SELECTION_SINGLE);
    g_signal_connect(dlg->list_box, "row-selected", G_CALLBACK(on_wifi_row_selected), dlg);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dlg->list_box);
    gtk_box_append(GTK_BOX(content_box), scrolled);

    /* Password Input Box (Hidden initially) */
    dlg->password_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
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
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_destroy), dlg->dialog_window);
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);

    dlg->connect_btn = gtk_button_new_with_label("Connect");
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
        GtkWidget *empty_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(empty_label),
            "<span color='#888'><i>Scanning for Wi-Fi adapters or NetworkManager not ready...</i></span>");
        gtk_widget_set_halign(empty_label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(self->devices_box), empty_label);
        if (devices != NULL) {
            g_ptr_array_unref(devices);
        }
        return;
    }

    for (guint i = 0; i < devices->len; i++) {
        MultiWifiDeviceInfo *info = g_ptr_array_index(devices, i);
        MultiWifiPathInfo *path = self->path_monitor ?
            multiwifi_path_monitor_find_path(self->path_monitor, info->ifname) : NULL;

        GtkWidget *card_frame = gtk_frame_new(NULL);
        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(card_box, 10);
        gtk_widget_set_margin_bottom(card_box, 10);
        gtk_widget_set_margin_start(card_box, 12);
        gtk_widget_set_margin_end(card_box, 12);

        /* Header row: Device Name, Type, and Path Status Badge */
        GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        const gchar *type_desc = "PCIe/Internal";
        if (info->ifname && strstr(info->ifname, "u")) {
            type_desc = "USB Adapter";
        }

        gchar *title_markup = g_strdup_printf(
            "<span size='medium' weight='bold'>%s</span>  <span size='small' color='#888'>[%s | %s]</span>",
            info->ifname ? info->ifname : "unknown",
            type_desc,
            info->hw_address ? info->hw_address : "no-mac"
        );
        GtkWidget *title_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(title_lbl), title_markup);
        gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(title_lbl, TRUE);
        gtk_box_append(GTK_BOX(header_row), title_lbl);
        g_free(title_markup);

        /* Status badge */
        GtkWidget *badge_lbl = gtk_label_new(NULL);
        if (path != NULL) {
            if (path->state == MULTIWIFI_PATH_STATE_ONLINE) {
                gtk_label_set_markup(GTK_LABEL(badge_lbl),
                    "<span foreground='#4caf50' weight='bold'>● Online</span>");
            } else if (path->state == MULTIWIFI_PATH_STATE_DEGRADED) {
                gtk_label_set_markup(GTK_LABEL(badge_lbl),
                    "<span foreground='#ff9800' weight='bold'>▲ Degraded</span>");
            } else if (path->state == MULTIWIFI_PATH_STATE_LOCAL_ONLY) {
                gtk_label_set_markup(GTK_LABEL(badge_lbl),
                    "<span foreground='#f44336' weight='bold'>✕ Local Only</span>");
            } else if (path->state == MULTIWIFI_PATH_STATE_PROBING) {
                gtk_label_set_markup(GTK_LABEL(badge_lbl),
                    "<span foreground='#2196f3'>◌ Probing</span>");
            } else {
                gtk_label_set_markup(GTK_LABEL(badge_lbl),
                    "<span foreground='#888888'>○ Offline</span>");
            }
        } else if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            gtk_label_set_markup(GTK_LABEL(badge_lbl),
                "<span foreground='#4caf50' weight='bold'>● Connected</span>");
        } else {
            gtk_label_set_markup(GTK_LABEL(badge_lbl),
                "<span foreground='#888888'>○ Disconnected</span>");
        }
        gtk_box_append(GTK_BOX(header_row), badge_lbl);
        gtk_box_append(GTK_BOX(card_box), header_row);

        /* Details row */
        GtkWidget *details_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(details_lbl), 0.0f);

        if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            double freq_ghz = (double)info->frequency / 1000.0;
            gchar *escaped_ssid = g_markup_escape_text(info->ssid ? info->ssid : "Hidden", -1);
            gchar *escaped_profile = g_markup_escape_text(info->connection_id ? info->connection_id : "Active", -1);
            gchar *details_markup = g_strdup_printf(
                "<b>SSID:</b> %s    <b>Signal:</b> %u%%    <b>Frequency:</b> %.1f GHz\n"
                "<b>IPv4:</b> %s    <b>Gateway:</b> %s    <b>Profile:</b> %s",
                escaped_ssid,
                info->signal_strength,
                freq_ghz,
                info->ipv4_address ? info->ipv4_address : "Pending DHCP",
                info->ipv4_gateway ? info->ipv4_gateway : "None",
                escaped_profile
            );
            gtk_label_set_markup(GTK_LABEL(details_lbl), details_markup);
            g_free(details_markup);
            g_free(escaped_ssid);
            g_free(escaped_profile);
        } else {
            gtk_label_set_markup(GTK_LABEL(details_lbl),
                "<span size='small' color='#777'><i>Adapter is idle. Ready to associate.</i></span>");
        }
        gtk_box_append(GTK_BOX(card_box), details_lbl);

        /* Path Metrics row (Phase 2 diagnostics) */
        if (path != NULL && path->state != MULTIWIFI_PATH_STATE_OFFLINE) {
            GtkWidget *metrics_lbl = gtk_label_new(NULL);
            gtk_label_set_xalign(GTK_LABEL(metrics_lbl), 0.0f);

            const gchar *eligibility_str = path->is_eligible
                ? "<span foreground='#4caf50'>Eligible</span>"
                : "<span foreground='#e91e63'>Excluded</span>";

            gchar *escaped_reason = g_markup_escape_text(path->status_reason ? path->status_reason : "N/A", -1);
            gchar *metrics_markup = g_strdup_printf(
                "<span size='small' color='#aaa'>"
                "<b>Path Diagnostics:</b> RTT: <b>%.1f ms</b> (min: %.1f, max: %.1f, jitter: %.1f) | "
                "Loss: <b>%u%%</b> | Status: <b>%s</b> | Steering: %s</span>",
                path->rtt_ms,
                path->min_rtt_ms,
                path->max_rtt_ms,
                path->jitter_ms,
                path->packet_loss_percent,
                escaped_reason,
                eligibility_str
            );
            gtk_label_set_markup(GTK_LABEL(metrics_lbl), metrics_markup);
            g_free(metrics_markup);
            g_free(escaped_reason);
            gtk_box_append(GTK_BOX(card_box), metrics_lbl);
        }

        /* Action row: Wi-Fi network selection */
        GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(action_row, 4);

        const gchar *btn_label = (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED)
            ? "Switch Wi-Fi Network..." : "Connect to Wi-Fi...";
        GtkWidget *wifi_btn = gtk_button_new_with_label(btn_label);
        gtk_widget_set_tooltip_text(wifi_btn, "Scan and select a Wi-Fi network for this adapter");
        g_object_set_data_full(G_OBJECT(wifi_btn), "ifname", g_strdup(info->ifname), g_free);
        g_signal_connect(wifi_btn, "clicked", G_CALLBACK(on_select_wifi_clicked), self);
        gtk_box_append(GTK_BOX(action_row), wifi_btn);
        gtk_box_append(GTK_BOX(card_box), action_row);

        if (path != NULL) {
            multiwifi_path_info_free(path);
        }

        gtk_frame_set_child(GTK_FRAME(card_frame), card_box);
        gtk_box_append(GTK_BOX(self->devices_box), card_frame);
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
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#388e3c'><b>System Daemon Active:</b> Connected via D-Bus. Privileged actions authorized via Polkit.</span>");

        gtk_widget_set_sensitive(self->lb_btn, !self->bonding_active);
        gtk_widget_set_sensitive(self->bonding_btn, !self->lb_active);

        if (self->lb_active) {
            gtk_button_set_label(GTK_BUTTON(self->lb_btn), "Stop Load Balancing");
            gtk_widget_set_tooltip_text(self->lb_btn, "Stop active multi-path policy routing and restore default route");
        } else {
            gtk_button_set_label(GTK_BUTTON(self->lb_btn), "Start Load Balancing");
            gtk_widget_set_tooltip_text(self->lb_btn, "Aggregate connected Wi-Fi uplinks with flow-affinity load balancing");
        }

        if (self->bonding_active) {
            gtk_button_set_label(GTK_BUTTON(self->bonding_btn), "Stop Bonding");
            gtk_widget_set_tooltip_text(self->bonding_btn, "Terminate MPTCP bonding tunnel");
        } else {
            gtk_button_set_label(GTK_BUTTON(self->bonding_btn), "Start Bonding");
            gtk_widget_set_tooltip_text(self->bonding_btn, "Establish MPTCP bonding tunnel through aggregation server");
        }
    } else {
        gtk_label_set_markup(GTK_LABEL(self->daemon_banner),
            "<span size='small' color='#d32f2f'><b>System Daemon Inactive:</b> Network mutation (Load Balancing &amp; Bonding) requires <tt>multiwifi-daemon</tt>.\n"
            "To enable, run: <tt>sudo systemctl enable --now multiwifi-daemon</tt></span>");

        gtk_widget_set_sensitive(self->lb_btn, FALSE);
        gtk_widget_set_sensitive(self->bonding_btn, FALSE);
        gtk_widget_set_tooltip_text(self->lb_btn, "Unavailable: MultiWiFi daemon is not running on system bus");
        gtk_widget_set_tooltip_text(self->bonding_btn, "Unavailable: MultiWiFi daemon is not running on system bus");
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
    if (self->daemon_status_lbl != NULL) {
        gtk_label_set_markup(GTK_LABEL(self->daemon_status_lbl),
            "<span foreground='#4caf50' weight='bold'>● Daemon Active (System D-Bus)</span>");
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
    if (self->daemon_status_lbl != NULL) {
        gtk_label_set_markup(GTK_LABEL(self->daemon_status_lbl),
            "<span foreground='#888888'>○ Daemon Standalone</span>");
    }
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
    gtk_window_set_title(GTK_WINDOW(self), "MultiWiFi");
    gtk_window_set_default_size(GTK_WINDOW(self), 720, 560);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(self), header);

    GtkWidget *diag_btn = gtk_button_new_with_label("Probe Paths");
    gtk_widget_set_tooltip_text(diag_btn, "Trigger path quality diagnostics cycle");
    g_signal_connect(diag_btn, "clicked", G_CALLBACK(on_diagnostics_clicked), self);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), diag_btn);

    self->daemon_status_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(self->daemon_status_lbl),
        "<span foreground='#888888'>○ Daemon Standalone</span>");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), self->daemon_status_lbl);

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

    /* Main Box */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(main_box, 16);
    gtk_widget_set_margin_bottom(main_box, 16);
    gtk_widget_set_margin_start(main_box, 16);
    gtk_widget_set_margin_end(main_box, 16);

    /* Welcome / Title Banner */
    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label),
        "<span size='x-large' weight='bold'>MultiWiFi Manager</span>\n"
        "<span size='small' color='#666'>Native Linux Multi-Path Uplink &amp; Bonding</span>");
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(main_box), title_label);

    /* Daemon Status Banner (informs users if daemon is absent or active) */
    self->daemon_banner = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(self->daemon_banner), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->daemon_banner), 0.0f);
    gtk_box_append(GTK_BOX(main_box), self->daemon_banner);

    /* Operations Action Card (Phase 7 packaging compliance) */
    GtkWidget *ops_frame = gtk_frame_new("Multi-Path Operations");
    GtkWidget *ops_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(ops_box, 10);
    gtk_widget_set_margin_bottom(ops_box, 10);
    gtk_widget_set_margin_start(ops_box, 12);
    gtk_widget_set_margin_end(ops_box, 12);

    self->lb_btn = gtk_button_new_with_label("Start Load Balancing");
    g_signal_connect(self->lb_btn, "clicked", G_CALLBACK(on_lb_clicked), self);
    gtk_box_append(GTK_BOX(ops_box), self->lb_btn);

    self->bonding_btn = gtk_button_new_with_label("Start Bonding");
    g_signal_connect(self->bonding_btn, "clicked", G_CALLBACK(on_bonding_clicked), self);
    gtk_box_append(GTK_BOX(ops_box), self->bonding_btn);

    gtk_frame_set_child(GTK_FRAME(ops_frame), ops_box);
    gtk_box_append(GTK_BOX(main_box), ops_frame);

    self->lb_active = FALSE;
    self->bonding_active = FALSE;
    update_control_buttons_state(self);

    /* Capabilities Card */
    GtkWidget *caps_frame = gtk_frame_new("System Capabilities");
    GtkWidget *caps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(caps_box, 12);
    gtk_widget_set_margin_bottom(caps_box, 12);
    gtk_widget_set_margin_start(caps_box, 12);
    gtk_widget_set_margin_end(caps_box, 12);

    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    multiwifi_capabilities_probe(caps);

    gchar *escaped_mptcp = g_markup_escape_text(caps->mptcp_details ? caps->mptcp_details : "none", -1);
    gchar *caps_text = g_strdup_printf(
        "<b>Kernel:</b> %s\n"
        "<b>NetworkManager:</b> %s\n"
        "<b>MPTCP Support:</b> %s (%s)\n"
        "<b>nftables / Netfilter:</b> %s\n"
        "<b>TUN Interface:</b> %s",
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
    gtk_box_append(GTK_BOX(caps_box), self->caps_label);
    gtk_frame_set_child(GTK_FRAME(caps_frame), caps_box);

    g_free(caps_text);
    multiwifi_capabilities_free(caps);

    gtk_box_append(GTK_BOX(main_box), caps_frame);

    /* Managed Wi-Fi Uplinks List */
    GtkWidget *status_frame = gtk_frame_new("Discovered Uplinks & Path Quality");
    self->devices_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(self->devices_box, 12);
    gtk_widget_set_margin_bottom(self->devices_box, 12);
    gtk_widget_set_margin_start(self->devices_box, 12);
    gtk_widget_set_margin_end(self->devices_box, 12);

    gtk_frame_set_child(GTK_FRAME(status_frame), self->devices_box);
    gtk_box_append(GTK_BOX(main_box), status_frame);

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

    /* Status Bar Footer */
    GtkWidget *footer_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_label),
        "<span size='small' color='#888'>MultiWiFi " MULTIWIFI_VERSION_STRING " | Unprivileged Mode | Multi-Path Uplink &amp; Bonding</span>");
    gtk_widget_set_halign(footer_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_box), footer_label);

    gtk_window_set_child(GTK_WINDOW(self), main_box);
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
