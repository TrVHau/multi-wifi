#include "window.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/version.h"

struct _MultiWifiWindow {
    GtkApplicationWindow parent_instance;
    GtkWidget *status_label;
    GtkWidget *caps_label;
};

G_DEFINE_TYPE(MultiWifiWindow, multiwifi_window, GTK_TYPE_APPLICATION_WINDOW)

static void multiwifi_window_init(MultiWifiWindow *self)
{
    gtk_window_set_title(GTK_WINDOW(self), "MultiWiFi");
    gtk_window_set_default_size(GTK_WINDOW(self), 640, 480);

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

    /* Inventory / Uplinks Card Placeholder */
    GtkWidget *status_frame = gtk_frame_new("Managed Wi-Fi Uplinks");
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(status_box, 12);
    gtk_widget_set_margin_bottom(status_box, 12);
    gtk_widget_set_margin_start(status_box, 12);
    gtk_widget_set_margin_end(status_box, 12);

    self->status_label = gtk_label_new(
        "Phase 0 Foundation initialized.\n"
        "Unprivileged desktop shell active. Ready for Phase 1 (Wi-Fi inventory)."
    );
    gtk_label_set_xalign(GTK_LABEL(self->status_label), 0.0f);
    gtk_box_append(GTK_BOX(status_box), self->status_label);
    gtk_frame_set_child(GTK_FRAME(status_frame), status_box);

    gtk_box_append(GTK_BOX(main_box), status_frame);

    /* Status Bar Footer */
    GtkWidget *footer_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_label),
        "<span size='small' color='#888'>MultiWiFi " MULTIWIFI_VERSION_STRING " | Unprivileged Mode</span>");
    gtk_widget_set_halign(footer_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(main_box), footer_label);

    gtk_window_set_child(GTK_WINDOW(self), main_box);
}

static void multiwifi_window_class_init(MultiWifiWindowClass *klass)
{
    (void)klass;
}

GtkWidget *multiwifi_window_new(GtkApplication *app)
{
    return g_object_new(MULTIWIFI_TYPE_WINDOW, "application", app, NULL);
}
