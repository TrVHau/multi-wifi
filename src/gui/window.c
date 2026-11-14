#include "window.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/version.h"

struct _MultiWifiWindow {
    GtkApplicationWindow parent_instance;
    GtkWidget *caps_label;
    GtkWidget *devices_box;
    MultiWifiInventory *inventory;
};

G_DEFINE_TYPE(MultiWifiWindow, multiwifi_window, GTK_TYPE_APPLICATION_WINDOW)

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

        GtkWidget *card_frame = gtk_frame_new(NULL);
        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(card_box, 10);
        gtk_widget_set_margin_bottom(card_box, 10);
        gtk_widget_set_margin_start(card_box, 12);
        gtk_widget_set_margin_end(card_box, 12);

        /* Header row: Device Name, Type, and Status Badge */
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
        if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            gtk_label_set_markup(GTK_LABEL(badge_lbl),
                "<span foreground='#4caf50' weight='bold'>● Connected</span>");
        } else if (info->state == MULTIWIFI_DEVICE_STATE_DISCONNECTED) {
            gtk_label_set_markup(GTK_LABEL(badge_lbl),
                "<span foreground='#ff9800' weight='bold'>○ Disconnected</span>");
        } else {
            gchar *state_markup = g_strdup_printf("<span foreground='#9e9e9e'>%s</span>",
                                                 info->state_str ? info->state_str : "Unknown");
            gtk_label_set_markup(GTK_LABEL(badge_lbl), state_markup);
            g_free(state_markup);
        }
        gtk_box_append(GTK_BOX(header_row), badge_lbl);
        gtk_box_append(GTK_BOX(card_box), header_row);

        /* Details row */
        GtkWidget *details_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(details_lbl), 0.0f);

        if (info->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
            double freq_ghz = (double)info->frequency / 1000.0;
            gchar *details_markup = g_strdup_printf(
                "<b>SSID:</b> %s    <b>Signal:</b> %u%%    <b>Frequency:</b> %.1f GHz\n"
                "<b>IPv4:</b> %s    <b>Gateway:</b> %s    <b>Profile:</b> %s",
                info->ssid ? info->ssid : "Hidden",
                info->signal_strength,
                freq_ghz,
                info->ipv4_address ? info->ipv4_address : "Pending DHCP",
                info->ipv4_gateway ? info->ipv4_gateway : "None",
                info->connection_id ? info->connection_id : "Active"
            );
            gtk_label_set_markup(GTK_LABEL(details_lbl), details_markup);
            g_free(details_markup);
        } else {
            gtk_label_set_markup(GTK_LABEL(details_lbl),
                "<span size='small' color='#777'><i>Adapter is idle. Ready to associate.</i></span>");
        }

        gtk_box_append(GTK_BOX(card_box), details_lbl);
        gtk_frame_set_child(GTK_FRAME(card_frame), card_box);
        gtk_box_append(GTK_BOX(self->devices_box), card_frame);
    }

    g_ptr_array_unref(devices);
}

static void on_inventory_devices_changed(MultiWifiInventory *inv, gpointer user_data)
{
    (void)inv;
    MultiWifiWindow *self = MULTIWIFI_WINDOW(user_data);
    update_devices_ui(self);
}

static void multiwifi_window_dispose(GObject *object)
{
    MultiWifiWindow *self = MULTIWIFI_WINDOW(object);

    if (self->inventory != NULL) {
        g_signal_handlers_disconnect_by_data(self->inventory, self);
        g_clear_object(&self->inventory);
    }

    G_OBJECT_CLASS(multiwifi_window_parent_class)->dispose(object);
}

static void multiwifi_window_init(MultiWifiWindow *self)
{
    gtk_window_set_title(GTK_WINDOW(self), "MultiWiFi");
    gtk_window_set_default_size(GTK_WINDOW(self), 700, 520);

    /* Header Bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(self), header);

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

    /* Capabilities Card */
    GtkWidget *caps_frame = gtk_frame_new("System Capabilities");
    GtkWidget *caps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(caps_box, 12);
    gtk_widget_set_margin_bottom(caps_box, 12);
    gtk_widget_set_margin_start(caps_box, 12);
    gtk_widget_set_margin_end(caps_box, 12);

    MultiWifiCapabilities *caps = multiwifi_capabilities_new();
    multiwifi_capabilities_probe(caps);

    gchar *caps_text = g_strdup_printf(
        "<b>Kernel:</b> %s\n"
        "<b>NetworkManager:</b> %s\n"
        "<b>MPTCP Support:</b> %s (%s)\n"
        "<b>nftables / Netfilter:</b> %s\n"
        "<b>TUN Interface:</b> %s",
        caps->kernel_version ? caps->kernel_version : "Unknown",
        multiwifi_cap_state_to_string(caps->network_manager),
        multiwifi_cap_state_to_string(caps->kernel_mptcp),
        caps->mptcp_details ? caps->mptcp_details : "none",
        multiwifi_cap_state_to_string(caps->nftables),
        multiwifi_cap_state_to_string(caps->tun_device)
    );

    self->caps_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(self->caps_label), caps_text);
    gtk_label_set_xalign(GTK_LABEL(self->caps_label), 0.0f);
    gtk_box_append(GTK_BOX(caps_box), self->caps_label);
    gtk_frame_set_child(GTK_FRAME(caps_frame), caps_box);

    g_free(caps_text);
    multiwifi_capabilities_free(caps);

    gtk_box_append(GTK_BOX(main_box), caps_frame);

    /* Managed Wi-Fi Uplinks List */
    GtkWidget *status_frame = gtk_frame_new("Discovered Wi-Fi Adapters");
    self->devices_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(self->devices_box, 12);
    gtk_widget_set_margin_bottom(self->devices_box, 12);
    gtk_widget_set_margin_start(self->devices_box, 12);
    gtk_widget_set_margin_end(self->devices_box, 12);

    gtk_frame_set_child(GTK_FRAME(status_frame), self->devices_box);
    gtk_box_append(GTK_BOX(main_box), status_frame);

    /* Initialize Inventory */
    self->inventory = multiwifi_inventory_new();
    g_signal_connect(self->inventory, "devices-changed",
                     G_CALLBACK(on_inventory_devices_changed), self);
    multiwifi_inventory_start(self->inventory, NULL);

    /* Initial UI render */
    update_devices_ui(self);

    /* Status Bar Footer */
    GtkWidget *footer_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_label),
        "<span size='small' color='#888'>MultiWiFi " MULTIWIFI_VERSION_STRING " | Unprivileged Mode | Phase 1 (Inventory)</span>");
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
