#include "multiwifi/daemon/service.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/core/logging.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/routing_manager.h"
#include "multiwifi/core/load_balancer.h"
#include "multiwifi/core/firewall.h"
#include "multiwifi/core/bonding_client.h"
#include "multiwifi/core/wifi_device.h"
#include "multiwifi/version.h"
#include <polkit/polkit.h>
#include <arpa/inet.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

static const gchar s_introspection_xml[] =
    "<node name=\"/io/github/multiwifi/Daemon\">"
    "  <interface name=\"io.github.multiwifi.Daemon1\">"
    "    <method name=\"GetVersion\">"
    "      <arg name=\"version\" type=\"s\" direction=\"out\"/>"
    "      <arg name=\"api_version\" type=\"u\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"GetCapabilities\">"
    "      <arg name=\"capabilities\" type=\"a{sv}\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"GetDevices\">"
    "      <arg name=\"devices\" type=\"aa{sv}\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"GetPaths\">"
    "      <arg name=\"paths\" type=\"aa{sv}\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"GetStatus\">"
    "      <arg name=\"status\" type=\"a{sv}\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"StartLoadBalancing\">"
    "      <arg name=\"options\" type=\"a{sv}\" direction=\"in\"/>"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"StopLoadBalancing\">"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"StartBonding\">"
    "      <arg name=\"profile\" type=\"a{sv}\" direction=\"in\"/>"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"StopBonding\">"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"SetPathWeight\">"
    "      <arg name=\"device_id\" type=\"s\" direction=\"in\"/>"
    "      <arg name=\"weight\" type=\"u\" direction=\"in\"/>"
    "    </method>"
    "    <method name=\"RunDiagnostics\">"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"GetDiagnostics\">"
    "      <arg name=\"operation_id\" type=\"s\" direction=\"in\"/>"
    "      <arg name=\"result\" type=\"a{sv}\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"StopDaemon\">"
    "    </method>"
    "    <method name=\"GetDNS\">"
    "      <arg name=\"servers\" type=\"as\" direction=\"out\"/>"
    "      <arg name=\"is_custom\" type=\"b\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"SetDNS\">"
    "      <arg name=\"servers\" type=\"as\" direction=\"in\"/>"
    "      <arg name=\"result\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <method name=\"ResetDNS\">"
    "      <arg name=\"result\" type=\"s\" direction=\"out\"/>"
    "    </method>"
    "    <signal name=\"DeviceChanged\">"
    "      <arg name=\"device\" type=\"a{sv}\"/>"
    "    </signal>"
    "    <signal name=\"PathChanged\">"
    "      <arg name=\"path\" type=\"a{sv}\"/>"
    "    </signal>"
    "    <signal name=\"StatusChanged\">"
    "      <arg name=\"status\" type=\"a{sv}\"/>"
    "    </signal>"
    "    <signal name=\"OperationProgress\">"
    "      <arg name=\"operation_id\" type=\"s\"/>"
    "      <arg name=\"percent\" type=\"u\"/>"
    "      <arg name=\"message\" type=\"s\"/>"
    "    </signal>"
    "    <signal name=\"OperationFinished\">"
    "      <arg name=\"operation_id\" type=\"s\"/>"
    "      <arg name=\"success\" type=\"b\"/>"
    "      <arg name=\"result\" type=\"a{sv}\"/>"
    "    </signal>"
    "    <signal name=\"Warning\">"
    "      <arg name=\"code\" type=\"s\"/>"
    "      <arg name=\"message\" type=\"s\"/>"
    "    </signal>"
    "  </interface>"
    "</node>";

struct _MultiWifiDaemonService {
    GObject parent_instance;
    GBusType bus_type;
    gboolean enable_polkit;

    GDBusConnection *connection;
    guint registration_id;
    guint owner_id;

    MultiWifiInventory *inventory;
    MultiWifiPathMonitor *path_monitor;
    MultiWifiOpManager *op_manager;
    MultiWifiRoutingManager *routing_mgr;
    MultiWifiScheduler *scheduler;
    MultiWifiFirewall *firewall;
    MultiWifiBondingClient *bonding_client;
    GDBusNodeInfo *node_info;
    GCancellable *cancellable;
    PolkitAuthority *authority;
    GArray *last_applied_marks;
    GHashTable *custom_weights;
    gint64 last_diagnostics_us;
    gboolean custom_dns_applied;
};

G_DEFINE_TYPE(MultiWifiDaemonService, multiwifi_daemon_service, G_TYPE_OBJECT)

static gboolean marks_equal(GArray *a, GArray *b)
{
    if (a == NULL || b == NULL) {
        return FALSE;
    }
    if (a->len != b->len || g_array_get_element_size(a) != g_array_get_element_size(b)) {
        return FALSE;
    }
    return memcmp(a->data, b->data, (gsize)a->len * g_array_get_element_size(a)) == 0;
}

static void cache_applied_marks(MultiWifiDaemonService *self, GArray *marks)
{
    g_clear_pointer(&self->last_applied_marks, g_array_unref);
    if (marks == NULL) {
        return;
    }
    guint elem_size = g_array_get_element_size(marks);
    self->last_applied_marks = g_array_sized_new(FALSE, FALSE, elem_size, marks->len);
    if (marks->len > 0) {
        g_array_append_vals(self->last_applied_marks, marks->data, marks->len);
    }
}

static void notify_nm_reload_async(MultiWifiDaemonService *self)
{
    if (self->bus_type != G_BUS_TYPE_SYSTEM || self->connection == NULL) {
        return;
    }
    if (g_getenv("MULTIWIFI_DNS_CONF_DIR") != NULL || g_getenv("MULTIWIFI_RESOLV_CONF") != NULL) {
        return;
    }
    g_dbus_connection_call(
        self->connection,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "Reload",
        g_variant_new("(u)", 0x3U),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        self->cancellable,
        NULL,
        NULL
    );
}

static GVariant *device_to_variant_dict(const MultiWifiDeviceInfo *d)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    g_variant_builder_add(&b, "{sv}", "ifname", g_variant_new_string(d->ifname ? d->ifname : ""));
    g_variant_builder_add(&b, "{sv}", "ifindex", g_variant_new_int32(d->ifindex));
    g_variant_builder_add(&b, "{sv}", "hw_address", g_variant_new_string(d->hw_address ? d->hw_address : ""));
    g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string(d->state_str ? d->state_str : ""));
    g_variant_builder_add(&b, "{sv}", "ssid", g_variant_new_string(d->ssid ? d->ssid : ""));
    g_variant_builder_add(&b, "{sv}", "bssid", g_variant_new_string(d->bssid ? d->bssid : ""));
    g_variant_builder_add(&b, "{sv}", "signal_strength", g_variant_new_byte(d->signal_strength));
    g_variant_builder_add(&b, "{sv}", "frequency", g_variant_new_uint32(d->frequency));
    g_variant_builder_add(&b, "{sv}", "ipv4_address", g_variant_new_string(d->ipv4_address ? d->ipv4_address : ""));
    g_variant_builder_add(&b, "{sv}", "ipv4_gateway", g_variant_new_string(d->ipv4_gateway ? d->ipv4_gateway : ""));
    g_variant_builder_add(&b, "{sv}", "ipv6_address", g_variant_new_string(d->ipv6_address ? d->ipv6_address : ""));
    g_variant_builder_add(&b, "{sv}", "connection_uuid", g_variant_new_string(d->connection_uuid ? d->connection_uuid : ""));
    g_variant_builder_add(&b, "{sv}", "connection_id", g_variant_new_string(d->connection_id ? d->connection_id : ""));
    g_variant_builder_add(&b, "{sv}", "managed", g_variant_new_boolean(d->managed));

    return g_variant_builder_end(&b);
}

static GVariant *path_to_variant_dict(const MultiWifiPathInfo *p)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    g_variant_builder_add(&b, "{sv}", "ifname", g_variant_new_string(p->ifname ? p->ifname : ""));
    g_variant_builder_add(&b, "{sv}", "gateway", g_variant_new_string(p->gateway ? p->gateway : ""));
    g_variant_builder_add(&b, "{sv}", "source_ip", g_variant_new_string(p->source_ip ? p->source_ip : ""));
    g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string(multiwifi_path_state_to_string(p->state)));
    g_variant_builder_add(&b, "{sv}", "status_reason", g_variant_new_string(p->status_reason ? p->status_reason : ""));
    g_variant_builder_add(&b, "{sv}", "rtt_ms", g_variant_new_double(p->rtt_ms));
    g_variant_builder_add(&b, "{sv}", "min_rtt_ms", g_variant_new_double(p->min_rtt_ms));
    g_variant_builder_add(&b, "{sv}", "max_rtt_ms", g_variant_new_double(p->max_rtt_ms));
    g_variant_builder_add(&b, "{sv}", "jitter_ms", g_variant_new_double(p->jitter_ms));
    g_variant_builder_add(&b, "{sv}", "loss_percent", g_variant_new_uint32(p->packet_loss_percent));
    g_variant_builder_add(&b, "{sv}", "eligible", g_variant_new_boolean(p->is_eligible));

    return g_variant_builder_end(&b);
}

static gboolean on_stop_daemon_idle(gpointer user_data)
{
    (void)user_data;
    raise(SIGTERM);
    return G_SOURCE_REMOVE;
}

static gchar *get_dns_nm_conf_path(void)
{
    const gchar *override = g_getenv("MULTIWIFI_DNS_CONF_DIR");
    if (override != NULL && *override != '\0') {
        return g_build_filename(override, "99-multiwifi-dns.conf", NULL);
    }
    return g_strdup("/etc/NetworkManager/conf.d/99-multiwifi-dns.conf");
}

static gchar *get_resolv_conf_path(void)
{
    const gchar *override = g_getenv("MULTIWIFI_RESOLV_CONF");
    if (override != NULL && *override != '\0') {
        return g_strdup(override);
    }
    return g_strdup("/etc/resolv.conf");
}

static gchar *get_resolv_backup_path(void)
{
    const gchar *override = g_getenv("MULTIWIFI_DNS_CONF_DIR");
    if (override != NULL && *override != '\0') {
        return g_build_filename(override, "resolv.conf.bak", NULL);
    }
    return g_strdup("/run/multiwifi/resolv.conf.bak");
}

static gboolean is_valid_dns_ip(const gchar *ip)
{
    if (ip == NULL || *ip == '\0') {
        return FALSE;
    }
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        return TRUE;
    }
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        return TRUE;
    }
    return FALSE;
}

static GPtrArray *read_current_dns_servers(void)
{
    GPtrArray *servers = g_ptr_array_new_with_free_func(g_free);
    gchar *resolv_path = get_resolv_conf_path();
    gchar *content = NULL;

    if (g_file_get_contents(resolv_path, &content, NULL, NULL) && content != NULL) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (gsize i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (g_str_has_prefix(line, "nameserver")) {
                gchar **parts = g_strsplit_set(line, " \t", -1);
                for (gsize j = 1; parts[j] != NULL; j++) {
                    gchar *ip = g_strstrip(parts[j]);
                    if (*ip != '\0' && is_valid_dns_ip(ip)) {
                        g_ptr_array_add(servers, g_strdup(ip));
                        break;
                    }
                }
                g_strfreev(parts);
            }
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(resolv_path);
    return servers;
}

static gboolean reject_if_symlink(const gchar *path, GError **error)
{
    if (path != NULL && g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Refusing to modify symbolic link target '%s'", path);
        return TRUE;
    }
    return FALSE;
}

static gboolean ensure_parent_dir(const gchar *file_path, GError **error)
{
    gchar *dir = g_path_get_dirname(file_path);
    int rc = g_mkdir_with_parents(dir, 0755);
    int saved_errno = errno;
    if (rc != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "Failed to create directory '%s': %s", dir, g_strerror(saved_errno));
        g_free(dir);
        return FALSE;
    }
    g_free(dir);
    return TRUE;
}

static gboolean apply_dns_configuration(GPtrArray *servers, GError **error)
{
    gchar *nm_path = get_dns_nm_conf_path();
    gchar *resolv_path = get_resolv_conf_path();
    gchar *backup_path = get_resolv_backup_path();
    gboolean ok = FALSE;

    if (reject_if_symlink(nm_path, error) || reject_if_symlink(backup_path, error)) {
        goto out;
    }

    /* 1. Write /etc/NetworkManager/conf.d/99-multiwifi-dns.conf */
    if (!ensure_parent_dir(nm_path, error)) {
        goto out;
    }

    GString *nm_content = g_string_new("");
    g_string_append(nm_content, "# Generated by MultiWiFi\n[global-dns]\nsearches=\n\n[global-dns-domain-*]\nservers=");
    for (guint i = 0; i < servers->len; i++) {
        if (i > 0) {
            g_string_append(nm_content, ",");
        }
        g_string_append(nm_content, (const gchar *)g_ptr_array_index(servers, i));
    }
    g_string_append(nm_content, "\n");

    if (!g_file_set_contents(nm_path, nm_content->str, -1, error)) {
        g_string_free(nm_content, TRUE);
        goto out;
    }
    g_string_free(nm_content, TRUE);

    /* 2. Preserve system symlinks (e.g. systemd-resolved stub) */
    if (g_file_test(resolv_path, G_FILE_TEST_IS_SYMLINK)) {
        ok = TRUE;
        goto out;
    }

    /* Only backup and directly modify resolv.conf if it is a regular static file */
    if (!g_file_test(backup_path, G_FILE_TEST_EXISTS) && g_file_test(resolv_path, G_FILE_TEST_EXISTS)) {
        if (!ensure_parent_dir(backup_path, error)) {
            goto out;
        }

        gchar *orig_content = NULL;
        if (!g_file_get_contents(resolv_path, &orig_content, NULL, error)) {
            goto out;
        }
        if (orig_content != NULL) {
            gboolean bak_ok = g_file_set_contents(backup_path, orig_content, -1, error);
            g_free(orig_content);
            if (!bak_ok) {
                goto out;
            }
        }
    }

    if (!ensure_parent_dir(resolv_path, error)) {
        goto out;
    }

    GString *resolv_content = g_string_new("");
    g_string_append(resolv_content, "# Generated by MultiWiFi\n");
    for (guint i = 0; i < servers->len; i++) {
        g_string_append_printf(resolv_content, "nameserver %s\n", (const gchar *)g_ptr_array_index(servers, i));
    }

    ok = g_file_set_contents(resolv_path, resolv_content->str, -1, error);
    g_string_free(resolv_content, TRUE);

out:
    g_free(nm_path);
    g_free(resolv_path);
    g_free(backup_path);
    return ok;
}

static gboolean reset_dns_configuration(GError **error)
{
    gchar *nm_path = get_dns_nm_conf_path();
    gchar *resolv_path = get_resolv_conf_path();
    gchar *backup_path = get_resolv_backup_path();
    gboolean ok = TRUE;

    /* 1. Remove NetworkManager override safely */
    if (g_file_test(nm_path, G_FILE_TEST_IS_SYMLINK)) {
        if (reject_if_symlink(nm_path, error)) {
            ok = FALSE;
            goto out;
        }
    } else if (g_file_test(nm_path, G_FILE_TEST_EXISTS)) {
        if (g_unlink(nm_path) != 0 && errno != ENOENT) {
            int saved_errno = errno;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                        "Failed to remove '%s': %s", nm_path, g_strerror(saved_errno));
            ok = FALSE;
            goto out;
        }
    }

    /* 2. Restore /etc/resolv.conf from backup if neither backup nor target is a symlink */
    if (g_file_test(backup_path, G_FILE_TEST_EXISTS)) {
        if (reject_if_symlink(backup_path, error) || reject_if_symlink(resolv_path, error)) {
            ok = FALSE;
            goto out;
        }
        gchar *backup_content = NULL;
        if (!g_file_get_contents(backup_path, &backup_content, NULL, error)) {
            ok = FALSE;
            goto out;
        }
        if (backup_content != NULL) {
            ok = g_file_set_contents(resolv_path, backup_content, -1, error);
            g_free(backup_content);
            if (!ok) {
                goto out;
            }
        }
        if (g_unlink(backup_path) != 0 && errno != ENOENT) {
            g_warning("Could not remove DNS backup '%s': %s", backup_path, g_strerror(errno));
        }
    }

out:
    g_free(nm_path);
    g_free(resolv_path);
    g_free(backup_path);
    return ok;
}

static GVariant *build_status_dict(MultiWifiDaemonService *self)
{
    gboolean is_active = multiwifi_routing_manager_is_active(self->routing_mgr);
    gboolean fw_active = multiwifi_firewall_is_active(self->firewall);
    MultiWifiBondingState bstate = multiwifi_bonding_client_get_state(self->bonding_client);
    gboolean bonding_active = (bstate == MULTIWIFI_BONDING_STATE_CONNECTED);
    guint eligible_paths = multiwifi_scheduler_get_eligible_count(self->scheduler);

    const gchar *mode_str = "idle";
    if (is_active) {
        mode_str = "load_balancing";
    } else if (bonding_active) {
        mode_str = "bonding";
    }

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "version", g_variant_new_string(MULTIWIFI_VERSION_STRING));
    g_variant_builder_add(&b, "{sv}", "mode", g_variant_new_string(mode_str));
    g_variant_builder_add(&b, "{sv}", "daemon_running", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&b, "{sv}", "routing_active", g_variant_new_boolean(is_active));
    g_variant_builder_add(&b, "{sv}", "firewall_active", g_variant_new_boolean(fw_active));
    g_variant_builder_add(&b, "{sv}", "bonding_active", g_variant_new_boolean(bonding_active));
    g_variant_builder_add(&b, "{sv}", "bonding_state", g_variant_new_string(multiwifi_bonding_client_state_to_string(bstate)));
    g_variant_builder_add(&b, "{sv}", "bonding_mptcp", g_variant_new_boolean(multiwifi_bonding_client_is_mptcp(self->bonding_client)));
    const gchar *vip = multiwifi_bonding_client_get_virtual_ip(self->bonding_client);
    g_variant_builder_add(&b, "{sv}", "bonding_virtual_ip", g_variant_new_string(vip ? vip : ""));
    g_variant_builder_add(&b, "{sv}", "eligible_uplinks", g_variant_new_uint32(eligible_paths));
    g_variant_builder_add(&b, "{sv}", "polkit_enforced", g_variant_new_boolean(self->enable_polkit));

    return g_variant_builder_end(&b);
}

static void emit_status_changed(MultiWifiDaemonService *self)
{
    if (self->connection != NULL) {
        GVariant *dict = build_status_dict(self);
        g_dbus_connection_emit_signal(
            self->connection,
            NULL,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "StatusChanged",
            g_variant_new("(@a{sv})", dict),
            NULL
        );
    }
}

static gboolean method_requires_polkit(const gchar *method_name)
{
    return (g_strcmp0(method_name, "StartLoadBalancing") == 0 ||
            g_strcmp0(method_name, "StopLoadBalancing") == 0 ||
            g_strcmp0(method_name, "StartBonding") == 0 ||
            g_strcmp0(method_name, "StopBonding") == 0 ||
            g_strcmp0(method_name, "SetPathWeight") == 0 ||
            g_strcmp0(method_name, "StopDaemon") == 0 ||
            g_strcmp0(method_name, "SetDNS") == 0 ||
            g_strcmp0(method_name, "ResetDNS") == 0);
}

static void dispatch_authorized_method(MultiWifiDaemonService *self,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *method_name,
                                       GVariant *parameters)
{
    if (g_strcmp0(method_name, "StartLoadBalancing") == 0) {
        if (multiwifi_bonding_client_get_state(self->bonding_client) == MULTIWIFI_BONDING_STATE_CONNECTED) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                "Cannot start load balancing while bonding is active"
            );
            return;
        }

        if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
            gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StartLoadBalancing");
            multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Load balancing is already active");
            emit_status_changed(self);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
            g_free(op_id);
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StartLoadBalancing");
        multiwifi_inventory_refresh(self->inventory);
        GPtrArray *devs = multiwifi_inventory_dup_devices(self->inventory);
        MultiWifiRoutePlan *plan = multiwifi_route_plan_new();

        if (devs != NULL) {
            uint32_t idx = 0;
            for (guint i = 0; i < devs->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devs, i);
                if (d->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
                    if (d->ipv4_address == NULL || *d->ipv4_address == '\0' ||
                        d->ipv4_gateway == NULL || *d->ipv4_gateway == '\0') {
                        multiwifi_wifi_device_fill_kernel_ipv4(d->ifname, &d->ipv4_address, &d->ipv4_gateway);
                    }
                }
                if (d->state == MULTIWIFI_DEVICE_STATE_CONNECTED &&
                    d->ipv4_address != NULL && *d->ipv4_address != '\0' &&
                    d->ipv4_gateway != NULL && *d->ipv4_gateway != '\0') {
                    multiwifi_route_plan_add_uplink(plan, d->ifname, (uint32_t)d->ifindex,
                                                    d->ipv4_address, d->ipv4_gateway, idx++);
                }
            }
            g_ptr_array_unref(devs);
        }

        if (plan->uplinks->len < 2) {
            guint count = plan->uplinks->len;
            multiwifi_route_plan_free(plan);
            multiwifi_op_manager_finish(self->op_manager, op_id, FALSE, NULL,
                                        "Load balancing requires at least 2 connected Wi-Fi interfaces with IPv4 default gateways");
            g_autofree gchar *err_msg = g_strdup_printf(
                "Cần tối thiểu 2 kết nối Wi-Fi hoạt động để thực hiện Cân bằng tải (hiện có %u)", count
            );
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE, err_msg
            );
            g_free(op_id);
            return;
        }

        GError *plan_err = NULL;
        if (!multiwifi_routing_manager_apply_plan(self->routing_mgr, op_id, plan, self->op_manager, &plan_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                plan_err ? plan_err->message : "Routing transaction failed"
            );
            g_clear_error(&plan_err);
            multiwifi_route_plan_free(plan);
            g_free(op_id);
            return;
        }

        /* Populate scheduler and apply firewall (preserving pre-set custom weights) */
        multiwifi_scheduler_reset(self->scheduler);
        for (guint i = 0; i < plan->uplinks->len; i++) {
            MultiWifiUplinkRouteConfig *up = g_ptr_array_index(plan->uplinks, i);
            MultiWifiPathInfo *p = multiwifi_path_monitor_find_path(self->path_monitor, up->ifname);
            gboolean eligible = TRUE;
            gdouble rtt = 10.0;
            guint loss = 0;
            if (p != NULL) {
                eligible = p->is_eligible;
                rtt = p->rtt_ms;
                loss = p->packet_loss_percent;
                multiwifi_path_info_free(p);
            }
            guint32 configured_weight = 100;
            if (self->custom_weights != NULL && up->ifname != NULL) {
                guint32 saved = GPOINTER_TO_UINT(g_hash_table_lookup(self->custom_weights, up->ifname));
                if (saved > 0) {
                    configured_weight = saved;
                }
            }
            multiwifi_scheduler_add_or_update(self->scheduler, up->ifname, up->ifname,
                                              up->fwmark, configured_weight, eligible, rtt, loss);
        }

        GArray *marks = NULL;
        guint n_marks = multiwifi_scheduler_get_distribution(self->scheduler, &marks);
        if (n_marks == 0 || marks == NULL) {
            multiwifi_routing_manager_teardown(self->routing_mgr, op_id, self->op_manager, NULL);
            multiwifi_route_plan_free(plan);
            if (marks != NULL) {
                g_array_unref(marks);
            }
            multiwifi_op_manager_finish(self->op_manager, op_id, FALSE, NULL,
                                        "No eligible uplinks available for load balancing");
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE,
                "No eligible uplinks available for load balancing"
            );
            g_free(op_id);
            return;
        }

        GError *fw_err = NULL;
        if (!multiwifi_firewall_apply(self->firewall, marks, &fw_err)) {
            g_array_unref(marks);
            multiwifi_routing_manager_teardown(self->routing_mgr, op_id, self->op_manager, NULL);
            multiwifi_route_plan_free(plan);
            multiwifi_op_manager_finish(self->op_manager, op_id, FALSE, NULL,
                                        fw_err ? fw_err->message : "Failed to apply firewall rules");
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE,
                fw_err ? fw_err->message : "Failed to apply firewall rules"
            );
            g_clear_error(&fw_err);
            g_free(op_id);
            return;
        }

        cache_applied_marks(self, marks);
        g_array_unref(marks);
        multiwifi_route_plan_free(plan);
        emit_status_changed(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StopLoadBalancing") == 0) {
        if (!multiwifi_routing_manager_is_active(self->routing_mgr)) {
            gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopLoadBalancing");
            multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Load balancing is already inactive");
            emit_status_changed(self);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
            g_free(op_id);
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopLoadBalancing");
        GError *fw_err = NULL;
        multiwifi_firewall_teardown(self->firewall, &fw_err);
        g_clear_error(&fw_err);
        cache_applied_marks(self, NULL);

        multiwifi_scheduler_reset(self->scheduler);

        GError *stop_err = NULL;
        if (!multiwifi_routing_manager_teardown(self->routing_mgr, op_id, self->op_manager, &stop_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_ROLLBACK_FAILED,
                stop_err ? stop_err->message : "Failed to teardown routing tables"
            );
            g_clear_error(&stop_err);
            g_free(op_id);
            return;
        }

        emit_status_changed(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StartBonding") == 0) {
        if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                "Cannot start bonding while load balancing is active"
            );
            return;
        }

        MultiWifiBondingState bstate = multiwifi_bonding_client_get_state(self->bonding_client);
        if (bstate == MULTIWIFI_BONDING_STATE_CONNECTED) {
            gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StartBonding");
            multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel is already active");
            emit_status_changed(self);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
            g_free(op_id);
            return;
        }
        if (bstate != MULTIWIFI_BONDING_STATE_DISCONNECTED && bstate != MULTIWIFI_BONDING_STATE_FAILED) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                "Bonding tunnel state transition is already in progress"
            );
            return;
        }

        GVariant *profile_dict = NULL;
        g_variant_get(parameters, "(@a{sv})", &profile_dict);

        gchar *server_host = NULL;
        GVariant *host_v = g_variant_lookup_value(profile_dict, "server_host", G_VARIANT_TYPE_STRING);
        if (host_v != NULL) {
            server_host = g_strstrip(g_strdup(g_variant_get_string(host_v, NULL)));
            g_variant_unref(host_v);
        }

        gchar *token = NULL;
        GVariant *tok_v = g_variant_lookup_value(profile_dict, "token", G_VARIANT_TYPE_STRING);
        if (tok_v != NULL) {
            token = g_strstrip(g_strdup(g_variant_get_string(tok_v, NULL)));
            g_variant_unref(tok_v);
        }

        guint32 server_port = 5000;
        GVariant *port_v = g_variant_lookup_value(profile_dict, "server_port", G_VARIANT_TYPE_UINT32);
        if (port_v != NULL) {
            server_port = g_variant_get_uint32(port_v);
            g_variant_unref(port_v);
        }

        gchar *profile_id = NULL;
        GVariant *pid_v = g_variant_lookup_value(profile_dict, "profile_id", G_VARIANT_TYPE_STRING);
        if (pid_v != NULL) {
            profile_id = g_strstrip(g_strdup(g_variant_get_string(pid_v, NULL)));
            g_variant_unref(pid_v);
        }
        if (profile_id == NULL || *profile_id == '\0') {
            g_free(profile_id);
            profile_id = g_strdup("default");
        }
        g_variant_unref(profile_dict);

        if (server_host == NULL || *server_host == '\0' || token == NULL || *token == '\0' ||
            server_port == 0 || server_port > 65535) {
            g_free(server_host);
            g_free(token);
            g_free(profile_id);
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_INVALID_ARGUMENT,
                "Invalid bonding profile: server_host, token, and valid server_port (1-65535) are required"
            );
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StartBonding");
        multiwifi_op_manager_update(self->op_manager, op_id, MULTIWIFI_OP_STATE_VALIDATING, 20,
                                    "Resolving host and verifying network uplinks");

        /* Find physical uplink for loop prevention */
        GPtrArray *devs = multiwifi_inventory_dup_devices(self->inventory);
        if (devs != NULL) {
            for (guint i = 0; i < devs->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devs, i);
                if (d->state == MULTIWIFI_DEVICE_STATE_CONNECTED &&
                    d->ipv4_gateway != NULL && *d->ipv4_gateway != '\0') {
                    multiwifi_bonding_client_set_uplink(self->bonding_client, d->ifname, d->ipv4_gateway);
                    break;
                }
            }
            g_ptr_array_unref(devs);
        }

        multiwifi_bonding_client_set_profile(self->bonding_client, profile_id, server_host, (uint16_t)server_port, token);
        g_free(server_host);
        g_free(token);
        g_free(profile_id);

        multiwifi_op_manager_update(self->op_manager, op_id, MULTIWIFI_OP_STATE_APPLYING, 50,
                                    "Connecting to aggregation server and establishing TUN");

        GError *start_err = NULL;
        if (!multiwifi_bonding_client_start(self->bonding_client, &start_err)) {
            multiwifi_op_manager_finish(self->op_manager, op_id, FALSE, NULL,
                                        start_err ? start_err->message : "Failed to establish bonding tunnel");
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE,
                start_err ? start_err->message : "Failed to establish bonding tunnel"
            );
            g_clear_error(&start_err);
            g_free(op_id);
            return;
        }

        multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel established successfully");
        emit_status_changed(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StopBonding") == 0) {
        if (multiwifi_bonding_client_get_state(self->bonding_client) == MULTIWIFI_BONDING_STATE_DISCONNECTED) {
            gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopBonding");
            multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel is already inactive");
            emit_status_changed(self);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
            g_free(op_id);
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopBonding");
        multiwifi_op_manager_update(self->op_manager, op_id, MULTIWIFI_OP_STATE_APPLYING, 50,
                                    "Tearing down bonding tunnel and restoring routes");

        multiwifi_bonding_client_stop(self->bonding_client);

        multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel terminated cleanly");
        emit_status_changed(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "SetPathWeight") == 0) {
        const gchar *dev_id = NULL;
        guint32 weight = 0;
        g_variant_get(parameters, "(&su)", &dev_id, &weight);

        if (dev_id == NULL || *dev_id == '\0' || weight < 1 || weight > 1000) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_INVALID_ARGUMENT,
                "Invalid parameters: weight must be between 1 and 1000 and device_id must be non-empty"
            );
            return;
        }

        if (self->custom_weights != NULL) {
            g_hash_table_replace(self->custom_weights, g_strdup(dev_id), GUINT_TO_POINTER(weight));
        }

        multiwifi_scheduler_set_weight(self->scheduler, dev_id, weight);
        if (multiwifi_firewall_is_active(self->firewall) &&
            multiwifi_routing_manager_is_active(self->routing_mgr)) {
            GArray *marks = NULL;
            guint n_marks = multiwifi_scheduler_get_distribution(self->scheduler, &marks);
            if (n_marks > 0 && marks != NULL && !marks_equal(self->last_applied_marks, marks)) {
                GError *fw_err = NULL;
                if (multiwifi_firewall_apply(self->firewall, marks, &fw_err)) {
                    cache_applied_marks(self, marks);
                }
                g_clear_error(&fw_err);
            }
            if (marks != NULL) {
                g_array_unref(marks);
            }
        }

        g_info("SetPathWeight: device '%s' weight set to %u", dev_id, weight);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "StopDaemon") == 0) {
        g_info("StopDaemon requested via D-Bus; scheduling clean termination");
        g_dbus_method_invocation_return_value(invocation, NULL);
        g_idle_add(on_stop_daemon_idle, NULL);
    } else if (g_strcmp0(method_name, "SetDNS") == 0) {
        GVariantIter *iter = NULL;
        g_variant_get(parameters, "(as)", &iter);
        GPtrArray *servers = g_ptr_array_new_with_free_func(g_free);
        const gchar *s = NULL;
        gboolean valid = TRUE;
        gboolean too_many = FALSE;

        while (g_variant_iter_next(iter, "&s", &s)) {
            if (servers->len >= 16) {
                too_many = TRUE;
                break;
            }
            gchar *trimmed = g_strstrip(g_strdup(s));
            if (!is_valid_dns_ip(trimmed)) {
                valid = FALSE;
                g_free(trimmed);
                break;
            }
            g_ptr_array_add(servers, trimmed);
        }
        g_variant_iter_free(iter);

        if (too_many || !valid || servers->len == 0) {
            g_ptr_array_unref(servers);
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_INVALID_ARGUMENT,
                too_many
                    ? "Too many DNS servers specified (maximum 16)"
                    : "Invalid DNS configuration: must provide 1-16 valid IPv4 or IPv6 addresses"
            );
            return;
        }

        GError *apply_err = NULL;
        if (!apply_dns_configuration(servers, &apply_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE,
                apply_err ? apply_err->message : "Failed to apply DNS configuration"
            );
            g_clear_error(&apply_err);
            g_ptr_array_unref(servers);
            return;
        }

        self->custom_dns_applied = TRUE;
        g_ptr_array_unref(servers);
        notify_nm_reload_async(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "OK"));
    } else if (g_strcmp0(method_name, "ResetDNS") == 0) {
        GError *reset_err = NULL;
        if (!reset_dns_configuration(&reset_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NETWORK_FAILURE,
                reset_err ? reset_err->message : "Failed to reset DNS configuration"
            );
            g_clear_error(&reset_err);
            return;
        }
        self->custom_dns_applied = FALSE;
        notify_nm_reload_async(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "OK"));
    } else {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown privileged method '%s'", method_name
        );
    }
}

typedef struct {
    MultiWifiDaemonService *service;
    GDBusMethodInvocation *invocation;
    gchar *method_name;
    GVariant *parameters;
    gchar *action_id;
    PolkitSubject *subject;
} PendingAuthCall;

static void pending_auth_call_free(PendingAuthCall *call)
{
    if (call == NULL) {
        return;
    }
    g_clear_object(&call->service);
    g_clear_object(&call->invocation);
    g_clear_pointer(&call->method_name, g_free);
    g_clear_pointer(&call->parameters, g_variant_unref);
    g_clear_pointer(&call->action_id, g_free);
    g_clear_object(&call->subject);
    g_free(call);
}

static void on_polkit_auth_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PendingAuthCall *call = (PendingAuthCall *)user_data;
    GError *error = NULL;
    PolkitAuthorizationResult *result = polkit_authority_check_authorization_finish(
        POLKIT_AUTHORITY(source), res, &error
    );

    if (result == NULL) {
        GDBusMethodInvocation *inv = call->invocation;
        call->invocation = NULL;
        if (error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
            (strstr(error->message, "not registered") != NULL ||
             !g_file_test("/usr/share/polkit-1/actions/io.github.multiwifi.policy", G_FILE_TEST_EXISTS))) {
            g_dbus_method_invocation_return_dbus_error(
                inv, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)"
            );
        } else {
            g_dbus_method_invocation_return_dbus_error(
                inv, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                error ? error->message : "Polkit authorization failed"
            );
        }
        g_clear_error(&error);
        pending_auth_call_free(call);
        return;
    }

    gboolean authorized = polkit_authorization_result_get_is_authorized(result);
    g_object_unref(result);

    if (!authorized) {
        GDBusMethodInvocation *inv = call->invocation;
        call->invocation = NULL;
        if (!g_file_test("/usr/share/polkit-1/actions/io.github.multiwifi.policy", G_FILE_TEST_EXISTS)) {
            g_dbus_method_invocation_return_dbus_error(
                inv, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)"
            );
        } else {
            g_autofree gchar *denied_msg = g_strdup_printf(
                "Not authorized by system policy for action '%s'", call->action_id
            );
            g_dbus_method_invocation_return_dbus_error(
                inv, MULTIWIFI_ERROR_NOT_AUTHORIZED, denied_msg
            );
        }
        pending_auth_call_free(call);
        return;
    }

    GDBusMethodInvocation *inv = call->invocation;
    call->invocation = NULL;
    dispatch_authorized_method(call->service, inv, call->method_name, call->parameters);
    pending_auth_call_free(call);
}

static void on_polkit_authority_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    PendingAuthCall *call = (PendingAuthCall *)user_data;
    GError *error = NULL;
    PolkitAuthority *authority = polkit_authority_get_finish(res, &error);

    if (authority == NULL) {
        GDBusMethodInvocation *inv = call->invocation;
        call->invocation = NULL;
        g_dbus_method_invocation_return_dbus_error(
            inv, MULTIWIFI_ERROR_NOT_AUTHORIZED,
            error ? error->message : "Polkit authority unavailable"
        );
        g_clear_error(&error);
        pending_auth_call_free(call);
        return;
    }

    if (call->service->authority == NULL) {
        call->service->authority = g_object_ref(authority);
    }

    polkit_authority_check_authorization(
        authority,
        call->subject,
        call->action_id,
        NULL,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
        call->service->cancellable,
        on_polkit_auth_ready,
        call
    );
    g_object_unref(authority);
}

static void begin_polkit_auth_and_dispatch(MultiWifiDaemonService *self,
                                           GDBusMethodInvocation *invocation,
                                           const gchar *method_name,
                                           GVariant *parameters,
                                           const gchar *action_id)
{
    if (!self->enable_polkit) {
        dispatch_authorized_method(self, invocation, method_name, parameters);
        return;
    }

    if (self->bus_type != G_BUS_TYPE_SYSTEM) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
            "Polkit authorization requires the system bus"
        );
        return;
    }

    const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
    if (sender == NULL) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
            "Caller has no D-Bus sender identity"
        );
        return;
    }

    PendingAuthCall *call = g_new0(PendingAuthCall, 1);
    call->service = g_object_ref(self);
    call->invocation = invocation;
    call->method_name = g_strdup(method_name);
    call->parameters = (parameters != NULL) ? g_variant_ref(parameters) : NULL;
    call->action_id = g_strdup(action_id);
    call->subject = polkit_system_bus_name_new(sender);

    if (self->authority != NULL) {
        polkit_authority_check_authorization(
            self->authority,
            call->subject,
            call->action_id,
            NULL,
            POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
            self->cancellable,
            on_polkit_auth_ready,
            call
        );
    } else {
        polkit_authority_get_async(self->cancellable, on_polkit_authority_ready, call);
    }
}

static void handle_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;

    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);

    if (method_requires_polkit(method_name)) {
        begin_polkit_auth_and_dispatch(self, invocation, method_name, parameters, "io.github.multiwifi.modify");
    } else if (g_strcmp0(method_name, "GetVersion") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(su)", MULTIWIFI_VERSION_STRING, (guint32)MULTIWIFI_API_VERSION)
        );
    } else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        MultiWifiCapabilities *caps = multiwifi_capabilities_new();
        multiwifi_capabilities_probe(caps);

        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "kernel_version", g_variant_new_string(caps->kernel_version ? caps->kernel_version : ""));
        g_variant_builder_add(&b, "{sv}", "mptcp_state", g_variant_new_string(multiwifi_cap_state_to_string(caps->kernel_mptcp)));
        g_variant_builder_add(&b, "{sv}", "mptcp_details", g_variant_new_string(caps->mptcp_details ? caps->mptcp_details : ""));
        g_variant_builder_add(&b, "{sv}", "nftables_state", g_variant_new_string(multiwifi_cap_state_to_string(caps->nftables)));
        g_variant_builder_add(&b, "{sv}", "tun_state", g_variant_new_string(multiwifi_cap_state_to_string(caps->tun_device)));
        g_variant_builder_add(&b, "{sv}", "network_manager_state", g_variant_new_string(multiwifi_cap_state_to_string(caps->network_manager)));

        multiwifi_capabilities_free(caps);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
    } else if (g_strcmp0(method_name, "GetDevices") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));

        multiwifi_inventory_refresh(self->inventory);
        GPtrArray *devices = multiwifi_inventory_dup_devices(self->inventory);
        if (devices != NULL) {
            for (guint i = 0; i < devices->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devices, i);
                if (d->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
                    if (d->ipv4_address == NULL || *d->ipv4_address == '\0' ||
                        d->ipv4_gateway == NULL || *d->ipv4_gateway == '\0') {
                        multiwifi_wifi_device_fill_kernel_ipv4(d->ifname, &d->ipv4_address, &d->ipv4_gateway);
                    }
                }
                g_variant_builder_add_value(&b, device_to_variant_dict(d));
            }
            g_ptr_array_unref(devices);
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@aa{sv})", g_variant_builder_end(&b)));
    } else if (g_strcmp0(method_name, "GetPaths") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));

        GPtrArray *paths = multiwifi_path_monitor_dup_paths(self->path_monitor);
        if (paths != NULL) {
            for (guint i = 0; i < paths->len; i++) {
                MultiWifiPathInfo *p = g_ptr_array_index(paths, i);
                g_variant_builder_add_value(&b, path_to_variant_dict(p));
            }
            g_ptr_array_unref(paths);
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@aa{sv})", g_variant_builder_end(&b)));
    } else if (g_strcmp0(method_name, "GetStatus") == 0) {
        GVariant *dict = build_status_dict(self);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", dict));
    } else if (g_strcmp0(method_name, "RunDiagnostics") == 0) {
        gint64 now_us = g_get_monotonic_time();
        if (self->last_diagnostics_us > 0 && (now_us - self->last_diagnostics_us) < 1500000) {
            /* Debounce/Rate-limit: prevent unprivileged D-Bus socket DoS spamming */
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_BUSY,
                "Diagnostics probe already in progress or ran recently. Please wait."
            );
            return;
        }
        self->last_diagnostics_us = now_us;

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "RunDiagnostics");
        multiwifi_op_manager_update(self->op_manager, op_id, MULTIWIFI_OP_STATE_APPLYING, 50, "Executing probe cycle");

        multiwifi_path_monitor_probe_all_async(self->path_monitor);
        multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Diagnostics cycle completed");

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "GetDiagnostics") == 0) {
        const gchar *op_id = NULL;
        g_variant_get(parameters, "(&s)", &op_id);

        GVariant *dict = multiwifi_op_manager_to_variant(self->op_manager, op_id);
        if (dict == NULL) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_INVALID_ARGUMENT,
                "Unknown or expired operation ID"
            );
            return;
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", dict));
    } else if (g_strcmp0(method_name, "GetDNS") == 0) {
        GPtrArray *servers = read_current_dns_servers();
        gchar *nm_path = get_dns_nm_conf_path();
        gboolean is_custom = g_file_test(nm_path, G_FILE_TEST_EXISTS);
        g_free(nm_path);

        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < servers->len; i++) {
            g_variant_builder_add(&b, "s", (const gchar *)g_ptr_array_index(servers, i));
        }
        g_ptr_array_unref(servers);

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(asb)", &b, is_custom));
    } else {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method '%s' on interface '%s'", method_name, interface_name
        );
    }
}

static const GDBusInterfaceVTable s_interface_vtable = {
    .method_call = handle_method_call,
    .get_property = NULL,
    .set_property = NULL,
    .padding = { 0 }
};

static void on_op_progress_forward(MultiWifiOpManager *mgr,
                                   const gchar *op_id,
                                   guint percent,
                                   const gchar *msg,
                                   gpointer user_data)
{
    (void)mgr;
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);
    if (self->connection != NULL) {
        g_dbus_connection_emit_signal(
            self->connection,
            NULL,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "OperationProgress",
            g_variant_new("(sus)", op_id, percent, msg ? msg : ""),
            NULL
        );
    }
}

static void on_op_finished_forward(MultiWifiOpManager *mgr,
                                   const gchar *op_id,
                                   gboolean success,
                                   GVariant *result,
                                   gpointer user_data)
{
    (void)mgr;
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);
    if (self->connection != NULL) {
        g_dbus_connection_emit_signal(
            self->connection,
            NULL,
            MULTIWIFI_DBUS_PATH,
            MULTIWIFI_DBUS_INTERFACE,
            "OperationFinished",
            g_variant_new("(sb@a{sv})", op_id, success, result),
            NULL
        );
    }
}

static void on_inventory_forward(MultiWifiInventory *inv, gpointer user_data)
{
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);
    if (self->connection != NULL && self->path_monitor != NULL) {
        GPtrArray *devs = multiwifi_inventory_dup_devices(inv);
        if (devs != NULL) {
            for (guint i = 0; i < devs->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devs, i);
                if (d->state == MULTIWIFI_DEVICE_STATE_CONNECTED) {
                    if (d->ipv4_address == NULL || *d->ipv4_address == '\0' ||
                        d->ipv4_gateway == NULL || *d->ipv4_gateway == '\0') {
                        multiwifi_wifi_device_fill_kernel_ipv4(d->ifname, &d->ipv4_address, &d->ipv4_gateway);
                    }
                }
                multiwifi_path_monitor_update_device(self->path_monitor, d);

                GVariant *dict = device_to_variant_dict(d);
                g_dbus_connection_emit_signal(
                    self->connection,
                    NULL,
                    MULTIWIFI_DBUS_PATH,
                    MULTIWIFI_DBUS_INTERFACE,
                    "DeviceChanged",
                    g_variant_new("(@a{sv})", dict),
                    NULL
                );
            }
            g_ptr_array_unref(devs);
        }
    }
}

static void on_paths_forward(MultiWifiPathMonitor *mon, gpointer user_data)
{
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);
    if (self->connection != NULL) {
        GPtrArray *paths = multiwifi_path_monitor_dup_paths(mon);
        if (paths != NULL) {
            for (guint i = 0; i < paths->len; i++) {
                MultiWifiPathInfo *p = g_ptr_array_index(paths, i);
                GVariant *dict = path_to_variant_dict(p);
                g_dbus_connection_emit_signal(
                    self->connection,
                    NULL,
                    MULTIWIFI_DBUS_PATH,
                    MULTIWIFI_DBUS_INTERFACE,
                    "PathChanged",
                    g_variant_new("(@a{sv})", dict),
                    NULL
                );

                if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
                    uint32_t fwmark = multiwifi_routing_manager_get_fwmark(self->routing_mgr, p->ifname);
                    if (fwmark > 0) {
                        multiwifi_scheduler_add_or_update(
                            self->scheduler, p->ifname, p->ifname, fwmark, 0,
                            p->is_eligible, p->rtt_ms, p->packet_loss_percent
                        );
                    }
                }
            }

            if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
                GArray *marks = NULL;
                guint n_marks = multiwifi_scheduler_get_distribution(self->scheduler, &marks);
                if (n_marks > 0 && marks != NULL) {
                    if (!multiwifi_firewall_is_active(self->firewall) ||
                        !marks_equal(self->last_applied_marks, marks)) {
                        GError *fw_err = NULL;
                        if (multiwifi_firewall_apply(self->firewall, marks, &fw_err)) {
                            cache_applied_marks(self, marks);
                        }
                        g_clear_error(&fw_err);
                    }
                } else if (multiwifi_firewall_is_active(self->firewall)) {
                    /* All paths down or ineligible: teardown firewall rules to prevent traffic blackholing */
                    GError *fw_err = NULL;
                    multiwifi_firewall_teardown(self->firewall, &fw_err);
                    g_clear_error(&fw_err);
                    cache_applied_marks(self, NULL);
                    g_warning("All load-balanced paths are down; suspended firewall mark distribution to prevent blackholing");
                }
                if (marks != NULL) {
                    g_array_unref(marks);
                }
            }

            g_ptr_array_unref(paths);
        }
    }
}

static void multiwifi_daemon_service_dispose(GObject *object)
{
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(object);
    multiwifi_daemon_service_stop(self);

    if (self->custom_dns_applied) {
        reset_dns_configuration(NULL);
        self->custom_dns_applied = FALSE;
    }
    if (self->firewall != NULL) {
        if (multiwifi_firewall_is_active(self->firewall)) {
            multiwifi_firewall_teardown(self->firewall, NULL);
        }
        g_clear_object(&self->firewall);
    }
    if (self->bonding_client != NULL) {
        multiwifi_bonding_client_free(self->bonding_client);
        self->bonding_client = NULL;
    }
    if (self->scheduler != NULL) {
        multiwifi_scheduler_free(self->scheduler);
        self->scheduler = NULL;
    }
    if (self->inventory != NULL) {
        g_signal_handlers_disconnect_by_data(self->inventory, self);
        g_clear_object(&self->inventory);
    }
    if (self->path_monitor != NULL) {
        g_signal_handlers_disconnect_by_data(self->path_monitor, self);
        g_clear_object(&self->path_monitor);
    }
    if (self->op_manager != NULL) {
        g_signal_handlers_disconnect_by_data(self->op_manager, self);
        g_clear_object(&self->op_manager);
    }
    if (self->routing_mgr != NULL) {
        if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
            multiwifi_routing_manager_teardown(self->routing_mgr, "daemon-dispose", NULL, NULL);
        }
        g_clear_object(&self->routing_mgr);
    }
    if (self->node_info != NULL) {
        g_dbus_node_info_unref(self->node_info);
        self->node_info = NULL;
    }
    if (self->cancellable != NULL) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    g_clear_object(&self->authority);
    g_clear_pointer(&self->last_applied_marks, g_array_unref);
    g_clear_pointer(&self->custom_weights, g_hash_table_destroy);

    G_OBJECT_CLASS(multiwifi_daemon_service_parent_class)->dispose(object);
}

static void multiwifi_daemon_service_class_init(MultiWifiDaemonServiceClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_daemon_service_dispose;
}

static void multiwifi_daemon_service_init(MultiWifiDaemonService *self)
{
    self->bus_type = G_BUS_TYPE_SYSTEM;
    self->enable_polkit = TRUE;
    self->connection = NULL;
    self->registration_id = 0;
    self->owner_id = 0;
    self->cancellable = g_cancellable_new();
    self->custom_weights = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    self->inventory = multiwifi_inventory_new();
    self->path_monitor = multiwifi_path_monitor_new();
    self->op_manager = multiwifi_op_manager_new();
    const gchar *jdir = (geteuid() == 0) ? "/run/multiwifi/journals" : NULL;
    self->routing_mgr = multiwifi_routing_manager_new(jdir);
    multiwifi_routing_manager_recover(self->routing_mgr, NULL);

    self->scheduler = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    self->firewall = multiwifi_firewall_new();
    self->bonding_client = multiwifi_bonding_client_new();

    /* Clean recovery of any stale firewall table from past abnormal termination */
    gboolean fw_exists = FALSE;
    if (multiwifi_firewall_verify(self->firewall, &fw_exists, NULL) && fw_exists) {
        multiwifi_firewall_teardown(self->firewall, NULL);
    }

    GError *err = NULL;
    self->node_info = g_dbus_node_info_new_for_xml(s_introspection_xml, &err);
    if (err != NULL) {
        g_critical("Failed to parse D-Bus introspection XML: %s", err->message);
        g_error_free(err);
    }

    g_signal_connect(self->op_manager, "operation-progress",
                     G_CALLBACK(on_op_progress_forward), self);
    g_signal_connect(self->op_manager, "operation-finished",
                     G_CALLBACK(on_op_finished_forward), self);
    g_signal_connect(self->inventory, "devices-changed",
                     G_CALLBACK(on_inventory_forward), self);
    g_signal_connect(self->path_monitor, "paths-changed",
                     G_CALLBACK(on_paths_forward), self);
}

MultiWifiDaemonService *multiwifi_daemon_service_new(GBusType bus_type, gboolean enable_polkit)
{
    MultiWifiDaemonService *svc = g_object_new(MULTIWIFI_TYPE_DAEMON_SERVICE, NULL);
    svc->bus_type = bus_type;
    svc->enable_polkit = enable_polkit;
    return svc;
}

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar *name,
                            gpointer user_data)
{
    MultiWifiDaemonService *self = MULTIWIFI_DAEMON_SERVICE(user_data);
    g_info("Acquired D-Bus connection for bus name '%s'", name);

    self->connection = g_object_ref(connection);
    GDBusInterfaceInfo *iface_info = self->node_info ? self->node_info->interfaces[0] : NULL;

    GError *error = NULL;
    self->registration_id = g_dbus_connection_register_object(
        connection,
        MULTIWIFI_DBUS_PATH,
        iface_info,
        &s_interface_vtable,
        self,
        NULL,
        &error
    );

    if (error != NULL) {
        g_critical("Failed to register D-Bus object: %s", error->message);
        g_error_free(error);
        return;
    }

    g_info("Registered D-Bus object path '%s' successfully", MULTIWIFI_DBUS_PATH);

    /* Start background inventory and path monitoring */
    multiwifi_inventory_start(self->inventory, NULL);
    multiwifi_path_monitor_start(self->path_monitor);
}

static void on_name_acquired(GDBusConnection *connection,
                             const gchar *name,
                             gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_info("Acquired D-Bus service name '%s'", name);
}

static void on_name_lost(GDBusConnection *connection,
                         const gchar *name,
                         gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_warning("Lost or failed to acquire D-Bus service name '%s'", name);
}

gboolean multiwifi_daemon_service_start(MultiWifiDaemonService *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Service is NULL");
        return FALSE;
    }

    if (self->owner_id > 0) {
        return TRUE;
    }

    self->owner_id = g_bus_own_name(
        self->bus_type,
        MULTIWIFI_DBUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        self,
        NULL
    );

    return TRUE;
}

void multiwifi_daemon_service_stop(MultiWifiDaemonService *self)
{
    if (self == NULL) {
        return;
    }

    if (self->cancellable != NULL) {
        g_cancellable_cancel(self->cancellable);
    }

    if (self->custom_dns_applied) {
        reset_dns_configuration(NULL);
        self->custom_dns_applied = FALSE;
    }

    if (self->firewall != NULL && multiwifi_firewall_is_active(self->firewall)) {
        multiwifi_firewall_teardown(self->firewall, NULL);
    }
    cache_applied_marks(self, NULL);
    if (self->routing_mgr != NULL && multiwifi_routing_manager_is_active(self->routing_mgr)) {
        multiwifi_routing_manager_teardown(self->routing_mgr, "daemon-stop", NULL, NULL);
    }
    if (self->bonding_client != NULL &&
        multiwifi_bonding_client_get_state(self->bonding_client) != MULTIWIFI_BONDING_STATE_DISCONNECTED) {
        multiwifi_bonding_client_stop(self->bonding_client);
    }

    if (self->registration_id > 0 && self->connection != NULL) {
        g_dbus_connection_unregister_object(self->connection, self->registration_id);
        self->registration_id = 0;
    }

    if (self->owner_id > 0) {
        g_bus_unown_name(self->owner_id);
        self->owner_id = 0;
    }

    g_clear_object(&self->connection);
}

GDBusConnection *multiwifi_daemon_service_get_connection(MultiWifiDaemonService *self)
{
    return self ? self->connection : NULL;
}
