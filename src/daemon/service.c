#include "multiwifi/daemon/service.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/core/logging.h"
#include "multiwifi/core/capabilities.h"
#include "multiwifi/core/routing_manager.h"
#include "multiwifi/core/load_balancer.h"
#include "multiwifi/core/firewall.h"
#include "multiwifi/core/bonding_client.h"
#include "multiwifi/version.h"
#include <polkit/polkit.h>
#include <arpa/inet.h>
#include <glib/gstdio.h>
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
    gint64 last_diagnostics_us;
    gboolean custom_dns_applied;
};

G_DEFINE_TYPE(MultiWifiDaemonService, multiwifi_daemon_service, G_TYPE_OBJECT)

static gboolean check_polkit_auth(MultiWifiDaemonService *self,
                                  GDBusMethodInvocation *invocation,
                                  const gchar *action_id,
                                  GError **error)
{
    if (!self->enable_polkit) {
        return TRUE;
    }

    const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
    if (sender == NULL) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "Caller has no D-Bus sender identity");
        return FALSE;
    }

    PolkitSubject *subject = polkit_system_bus_name_new(sender);
    PolkitAuthority *authority = polkit_authority_get_sync(NULL, error);
    if (authority == NULL) {
        g_object_unref(subject);
        return FALSE;
    }

    PolkitAuthorizationResult *result = polkit_authority_check_authorization_sync(
        authority,
        subject,
        action_id,
        NULL,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
        NULL,
        error
    );

    g_object_unref(authority);
    g_object_unref(subject);

    if (result == NULL) {
        if (error != NULL && *error != NULL) {
            if (strstr((*error)->message, "not registered") != NULL ||
                !g_file_test("/usr/share/polkit-1/actions/io.github.multiwifi.policy", G_FILE_TEST_EXISTS)) {
                g_clear_error(error);
                g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                            "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)");
            }
        }
        return FALSE;
    }

    gboolean authorized = polkit_authorization_result_get_is_authorized(result);
    g_object_unref(result);

    if (!authorized) {
        if (!g_file_test("/usr/share/polkit-1/actions/io.github.multiwifi.policy", G_FILE_TEST_EXISTS)) {
            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                        "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)");
        } else {
            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                        "Not authorized by system policy for action '%s'", action_id);
        }
        return FALSE;
    }

    return TRUE;
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

static gboolean apply_dns_configuration(GPtrArray *servers, GError **error)
{
    gchar *nm_path = get_dns_nm_conf_path();
    gchar *resolv_path = get_resolv_conf_path();
    gchar *backup_path = get_resolv_backup_path();

    /* 1. Backup resolv.conf if not already backed up */
    if (!g_file_test(backup_path, G_FILE_TEST_EXISTS) && g_file_test(resolv_path, G_FILE_TEST_EXISTS)) {
        gchar *parent_dir = g_path_get_dirname(backup_path);
        g_mkdir_with_parents(parent_dir, 0755);
        g_free(parent_dir);

        gchar *orig_content = NULL;
        if (g_file_get_contents(resolv_path, &orig_content, NULL, NULL) && orig_content != NULL) {
            g_file_set_contents(backup_path, orig_content, -1, NULL);
            g_free(orig_content);
        }
    }

    /* 2. Write /etc/NetworkManager/conf.d/99-multiwifi-dns.conf */
    gchar *nm_dir = g_path_get_dirname(nm_path);
    g_mkdir_with_parents(nm_dir, 0755);
    g_free(nm_dir);

    GString *nm_content = g_string_new("");
    g_string_append(nm_content, "# Generated by MultiWiFi\n[global-dns]\nsearches=\n\n[global-dns-domain-*]\nservers=");
    for (guint i = 0; i < servers->len; i++) {
        if (i > 0) g_string_append(nm_content, ",");
        g_string_append(nm_content, (const gchar *)g_ptr_array_index(servers, i));
    }
    g_string_append(nm_content, "\n");

    g_file_set_contents(nm_path, nm_content->str, -1, NULL);
    g_string_free(nm_content, TRUE);

    /* 3. Write /etc/resolv.conf */
    gchar *resolv_dir = g_path_get_dirname(resolv_path);
    g_mkdir_with_parents(resolv_dir, 0755);
    g_free(resolv_dir);

    GString *resolv_content = g_string_new("");
    g_string_append(resolv_content, "# Generated by MultiWiFi\n");
    for (guint i = 0; i < servers->len; i++) {
        g_string_append_printf(resolv_content, "nameserver %s\n", (const gchar *)g_ptr_array_index(servers, i));
    }

    gboolean ok = g_file_set_contents(resolv_path, resolv_content->str, -1, error);
    g_string_free(resolv_content, TRUE);

    g_free(nm_path);
    g_free(resolv_path);
    g_free(backup_path);
    return ok;
}

static gboolean reset_dns_configuration(GError **error)
{
    (void)error;
    gchar *nm_path = get_dns_nm_conf_path();
    gchar *resolv_path = get_resolv_conf_path();
    gchar *backup_path = get_resolv_backup_path();

    /* 1. Remove NetworkManager override */
    if (g_file_test(nm_path, G_FILE_TEST_EXISTS)) {
        g_unlink(nm_path);
    }

    /* 2. Restore /etc/resolv.conf from backup */
    if (g_file_test(backup_path, G_FILE_TEST_EXISTS)) {
        gchar *backup_content = NULL;
        if (g_file_get_contents(backup_path, &backup_content, NULL, NULL) && backup_content != NULL) {
            g_file_set_contents(resolv_path, backup_content, -1, NULL);
            g_free(backup_content);
        }
        g_unlink(backup_path);
    }

    g_free(nm_path);
    g_free(resolv_path);
    g_free(backup_path);
    return TRUE;
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

    if (g_strcmp0(method_name, "GetVersion") == 0) {
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

        GPtrArray *devices = multiwifi_inventory_dup_devices(self->inventory);
        if (devices != NULL) {
            for (guint i = 0; i < devices->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devices, i);
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

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
    } else if (g_strcmp0(method_name, "StartLoadBalancing") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        if (multiwifi_bonding_client_get_state(self->bonding_client) == MULTIWIFI_BONDING_STATE_CONNECTED) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                "Cannot start load balancing while bonding is active"
            );
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StartLoadBalancing");
        GPtrArray *devs = multiwifi_inventory_dup_devices(self->inventory);
        MultiWifiRoutePlan *plan = multiwifi_route_plan_new();

        if (devs != NULL) {
            uint32_t idx = 0;
            for (guint i = 0; i < devs->len; i++) {
                MultiWifiDeviceInfo *d = g_ptr_array_index(devs, i);
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

        /* Populate scheduler and apply firewall */
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
            multiwifi_scheduler_add_or_update(self->scheduler, up->ifname, up->ifname,
                                              up->fwmark, 100, eligible, rtt, loss);
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

        g_array_unref(marks);
        multiwifi_route_plan_free(plan);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StopLoadBalancing") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopLoadBalancing");
        GError *fw_err = NULL;
        multiwifi_firewall_teardown(self->firewall, &fw_err);
        g_clear_error(&fw_err);

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

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StartBonding") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        if (multiwifi_routing_manager_is_active(self->routing_mgr)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_CONFLICT,
                "Cannot start bonding while load balancing is active"
            );
            return;
        }

        if (multiwifi_bonding_client_get_state(self->bonding_client) == MULTIWIFI_BONDING_STATE_CONNECTED) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_BUSY,
                "Bonding tunnel is already active"
            );
            return;
        }

        GVariant *profile_dict = NULL;
        g_variant_get(parameters, "(@a{sv})", &profile_dict);

        const gchar *server_host = NULL;
        GVariant *host_v = g_variant_lookup_value(profile_dict, "server_host", G_VARIANT_TYPE_STRING);
        if (host_v != NULL) {
            server_host = g_variant_get_string(host_v, NULL);
        }

        const gchar *token = NULL;
        GVariant *tok_v = g_variant_lookup_value(profile_dict, "token", G_VARIANT_TYPE_STRING);
        if (tok_v != NULL) {
            token = g_variant_get_string(tok_v, NULL);
        }

        guint32 server_port = 5000;
        GVariant *port_v = g_variant_lookup_value(profile_dict, "server_port", G_VARIANT_TYPE_UINT32);
        if (port_v != NULL) {
            server_port = g_variant_get_uint32(port_v);
        }

        const gchar *profile_id = "default";
        GVariant *pid_v = g_variant_lookup_value(profile_dict, "profile_id", G_VARIANT_TYPE_STRING);
        if (pid_v != NULL) {
            profile_id = g_variant_get_string(pid_v, NULL);
        }

        if (server_host == NULL || *server_host == '\0' || token == NULL || *token == '\0' ||
            server_port == 0 || server_port > 65535) {
            if (host_v) g_variant_unref(host_v);
            if (tok_v) g_variant_unref(tok_v);
            if (port_v) g_variant_unref(port_v);
            if (pid_v) g_variant_unref(pid_v);
            g_variant_unref(profile_dict);
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
            if (host_v) g_variant_unref(host_v);
            if (tok_v) g_variant_unref(tok_v);
            if (port_v) g_variant_unref(port_v);
            if (pid_v) g_variant_unref(pid_v);
            g_variant_unref(profile_dict);
            g_free(op_id);
            return;
        }

        if (host_v) g_variant_unref(host_v);
        if (tok_v) g_variant_unref(tok_v);
        if (port_v) g_variant_unref(port_v);
        if (pid_v) g_variant_unref(pid_v);
        g_variant_unref(profile_dict);

        multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel established successfully");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "StopBonding") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        gchar *op_id = multiwifi_op_manager_create_op(self->op_manager, "StopBonding");
        multiwifi_op_manager_update(self->op_manager, op_id, MULTIWIFI_OP_STATE_APPLYING, 50,
                                    "Tearing down bonding tunnel and restoring routes");

        multiwifi_bonding_client_stop(self->bonding_client);

        multiwifi_op_manager_finish(self->op_manager, op_id, TRUE, NULL, "Bonding tunnel terminated cleanly");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", op_id));
        g_free(op_id);
    } else if (g_strcmp0(method_name, "SetPathWeight") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

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

        multiwifi_scheduler_set_weight(self->scheduler, dev_id, weight);
        if (multiwifi_firewall_is_active(self->firewall) &&
            multiwifi_routing_manager_is_active(self->routing_mgr)) {
            GArray *marks = NULL;
            guint n_marks = multiwifi_scheduler_get_distribution(self->scheduler, &marks);
            if (n_marks > 0 && marks != NULL) {
                GError *fw_err = NULL;
                multiwifi_firewall_apply(self->firewall, marks, &fw_err);
                g_clear_error(&fw_err);
            }
            if (marks != NULL) {
                g_array_unref(marks);
            }
        }

        g_info("SetPathWeight: device '%s' weight set to %u", dev_id, weight);
        g_dbus_method_invocation_return_value(invocation, NULL);
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
    } else if (g_strcmp0(method_name, "StopDaemon") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        g_info("StopDaemon requested via D-Bus; scheduling clean termination");
        g_dbus_method_invocation_return_value(invocation, NULL);
        g_idle_add(on_stop_daemon_idle, NULL);
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
    } else if (g_strcmp0(method_name, "SetDNS") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        GVariantIter *iter = NULL;
        g_variant_get(parameters, "(as)", &iter);
        GPtrArray *servers = g_ptr_array_new_with_free_func(g_free);
        const gchar *s = NULL;
        gboolean valid = TRUE;

        while (g_variant_iter_next(iter, "&s", &s)) {
            gchar *trimmed = g_strstrip(g_strdup(s));
            if (!is_valid_dns_ip(trimmed)) {
                valid = FALSE;
                g_free(trimmed);
                break;
            }
            g_ptr_array_add(servers, trimmed);
        }
        g_variant_iter_free(iter);

        if (!valid || servers->len == 0) {
            g_ptr_array_unref(servers);
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_INVALID_ARGUMENT,
                "Invalid DNS configuration: must provide at least one valid IPv4 or IPv6 address"
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
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "OK"));
    } else if (g_strcmp0(method_name, "ResetDNS") == 0) {
        GError *auth_err = NULL;
        if (!check_polkit_auth(self, invocation, "io.github.multiwifi.modify", &auth_err)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, MULTIWIFI_ERROR_NOT_AUTHORIZED,
                auth_err ? auth_err->message : "Permission denied"
            );
            g_clear_error(&auth_err);
            return;
        }

        reset_dns_configuration(NULL);
        self->custom_dns_applied = FALSE;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "OK"));
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
                    GError *fw_err = NULL;
                    multiwifi_firewall_apply(self->firewall, marks, &fw_err);
                    g_clear_error(&fw_err);
                } else if (multiwifi_firewall_is_active(self->firewall)) {
                    /* All paths down or ineligible: teardown firewall rules to prevent traffic blackholing */
                    GError *fw_err = NULL;
                    multiwifi_firewall_teardown(self->firewall, &fw_err);
                    g_clear_error(&fw_err);
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

    self->inventory = multiwifi_inventory_new();
    self->path_monitor = multiwifi_path_monitor_new();
    self->op_manager = multiwifi_op_manager_new();
    self->routing_mgr = multiwifi_routing_manager_new(NULL);
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

    if (self->custom_dns_applied) {
        reset_dns_configuration(NULL);
        self->custom_dns_applied = FALSE;
    }

    if (self->firewall != NULL && multiwifi_firewall_is_active(self->firewall)) {
        multiwifi_firewall_teardown(self->firewall, NULL);
    }
    if (self->routing_mgr != NULL && multiwifi_routing_manager_is_active(self->routing_mgr)) {
        multiwifi_routing_manager_teardown(self->routing_mgr, "daemon-stop", NULL, NULL);
    }
    if (self->bonding_client != NULL &&
        multiwifi_bonding_client_get_state(self->bonding_client) == MULTIWIFI_BONDING_STATE_CONNECTED) {
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
