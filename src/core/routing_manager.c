#include "multiwifi/core/routing_manager.h"
#include "multiwifi/core/netlink_route.h"
#include "multiwifi/core/logging.h"
#include <gio/gio.h>
#include <arpa/inet.h>
#include <string.h>

struct _MultiWifiRoutingManager {
    GObject parent_instance;
    gchar *journal_dir;
    MultiWifiRoutePlan *active_plan;
    gboolean is_active;
    GMutex mutex;
};

G_DEFINE_TYPE(MultiWifiRoutingManager, multiwifi_routing_manager, G_TYPE_OBJECT)

static void uplink_config_free(MultiWifiUplinkRouteConfig *cfg)
{
    if (cfg == NULL) {
        return;
    }
    g_free(cfg->ifname);
    g_free(cfg->ipv4_address);
    g_free(cfg->gateway);
    g_free(cfg);
}

MultiWifiRoutePlan *multiwifi_route_plan_new(void)
{
    MultiWifiRoutePlan *plan = g_new0(MultiWifiRoutePlan, 1);
    plan->uplinks = g_ptr_array_new_with_free_func((GDestroyNotify)uplink_config_free);
    return plan;
}

void multiwifi_route_plan_add_uplink(MultiWifiRoutePlan *plan,
                                     const gchar *ifname,
                                     uint32_t ifindex,
                                     const gchar *ipv4_addr,
                                     const gchar *gateway,
                                     uint32_t index)
{
    if (plan == NULL || ifname == NULL) {
        return;
    }

    MultiWifiUplinkRouteConfig *cfg = g_new0(MultiWifiUplinkRouteConfig, 1);
    cfg->ifname = g_strdup(ifname);
    cfg->ifindex = ifindex;
    cfg->ipv4_address = g_strdup(ipv4_addr);
    cfg->gateway = g_strdup(gateway);

    cfg->table_id = MULTIWIFI_BASE_TABLE_ID + (index + 1);
    cfg->src_rule_pref = MULTIWIFI_BASE_RULE_PREF + (index + 1);
    cfg->fwmark_rule_pref = MULTIWIFI_BASE_RULE_PREF + 1000 + (index + 1);
    cfg->fwmark = MULTIWIFI_BASE_FWMARK + (index + 1);
    cfg->fwmask = MULTIWIFI_DEFAULT_FWMASK;
    cfg->metric = 100 + (index * 10);

    g_ptr_array_add(plan->uplinks, cfg);
}

void multiwifi_route_plan_free(MultiWifiRoutePlan *plan)
{
    if (plan == NULL) {
        return;
    }
    if (plan->uplinks != NULL) {
        g_ptr_array_unref(plan->uplinks);
    }
    g_free(plan);
}

static void multiwifi_routing_manager_dispose(GObject *object)
{
    MultiWifiRoutingManager *self = MULTIWIFI_ROUTING_MANAGER(object);

    g_mutex_lock(&self->mutex);
    if (self->active_plan != NULL) {
        multiwifi_route_plan_free(self->active_plan);
        self->active_plan = NULL;
    }
    g_free(self->journal_dir);
    self->journal_dir = NULL;
    g_mutex_unlock(&self->mutex);

    G_OBJECT_CLASS(multiwifi_routing_manager_parent_class)->dispose(object);
}

static void multiwifi_routing_manager_finalize(GObject *object)
{
    MultiWifiRoutingManager *self = MULTIWIFI_ROUTING_MANAGER(object);
    g_mutex_clear(&self->mutex);
    G_OBJECT_CLASS(multiwifi_routing_manager_parent_class)->finalize(object);
}

static void multiwifi_routing_manager_class_init(MultiWifiRoutingManagerClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_routing_manager_dispose;
    oclass->finalize = multiwifi_routing_manager_finalize;
}

static void multiwifi_routing_manager_init(MultiWifiRoutingManager *self)
{
    g_mutex_init(&self->mutex);
    self->journal_dir = g_build_filename(g_get_user_runtime_dir(), "multiwifi", "journals", NULL);
    self->active_plan = NULL;
    self->is_active = FALSE;
}

MultiWifiRoutingManager *multiwifi_routing_manager_new(const gchar *journal_dir)
{
    MultiWifiRoutingManager *mgr = g_object_new(MULTIWIFI_TYPE_ROUTING_MANAGER, NULL);
    if (journal_dir != NULL && *journal_dir != '\0') {
        g_free(mgr->journal_dir);
        mgr->journal_dir = g_strdup(journal_dir);
    }
    return mgr;
}

gboolean multiwifi_routing_manager_check_conflicts(MultiWifiRoutingManager *self,
                                                   const MultiWifiRoutePlan *plan,
                                                   GError **error)
{
    if (self == NULL || plan == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to check conflicts");
        return FALSE;
    }

    if (plan->uplinks->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Route plan has no uplinks configured");
        return FALSE;
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd < 0) {
        return FALSE;
    }

    for (guint i = 0; i < plan->uplinks->len; i++) {
        MultiWifiUplinkRouteConfig *u = g_ptr_array_index(plan->uplinks, i);

        if (u->ifname == NULL || *u->ifname == '\0') {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Uplink has empty interface name");
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }

        if (u->gateway == NULL || *u->gateway == '\0') {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Uplink '%s' has no default gateway", u->ifname);
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }

        struct in_addr gw_test;
        if (inet_pton(AF_INET, u->gateway, &gw_test) != 1) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Uplink '%s' gateway '%s' is not a valid IPv4 address", u->ifname, u->gateway);
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }

        if (u->table_id <= 255) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Table ID %u collides with system reserved range (<= 255)", u->table_id);
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }

        /* Check if proposed routing table already has active routes */
        gboolean has_routes = FALSE;
        if (!multiwifi_netlink_table_has_routes(nl_fd, u->table_id, &has_routes, error)) {
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }
        if (has_routes) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Routing conflict: Table ID %u already contains active routes in kernel", u->table_id);
            multiwifi_netlink_close(nl_fd);
            return FALSE;
        }

        /* Check if proposed rule preference already exists in kernel */
        if (u->ipv4_address != NULL && *u->ipv4_address != '\0') {
            gboolean rule_exists = FALSE;
            if (!multiwifi_netlink_verify_rule(nl_fd, u->table_id, u->src_rule_pref, &rule_exists, error)) {
                multiwifi_netlink_close(nl_fd);
                return FALSE;
            }
            if (rule_exists) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Routing conflict: Rule priority %u already exists in RPDB", u->src_rule_pref);
                multiwifi_netlink_close(nl_fd);
                return FALSE;
            }
        }

        if (u->fwmark > 0) {
            gboolean fwrule_exists = FALSE;
            if (!multiwifi_netlink_verify_rule(nl_fd, u->table_id, u->fwmark_rule_pref, &fwrule_exists, error)) {
                multiwifi_netlink_close(nl_fd);
                return FALSE;
            }
            if (fwrule_exists) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Routing conflict: Rule priority %u already exists in RPDB", u->fwmark_rule_pref);
                multiwifi_netlink_close(nl_fd);
                return FALSE;
            }
        }
    }

    multiwifi_netlink_close(nl_fd);
    return TRUE;
}

static void execute_journal_rollback(int nl_fd, MultiWifiRouteJournal *journal)
{
    if (nl_fd < 0 || journal == NULL) {
        return;
    }

    GPtrArray *entries = multiwifi_route_journal_get_entries(journal);
    if (entries == NULL) {
        return;
    }

    g_info("Initiating atomic rollback for operation '%s' (%u entries to rewind)...",
           multiwifi_route_journal_get_op_id(journal), entries->len);

    /* Rewind in reverse order */
    for (int i = (int)entries->len - 1; i >= 0; i--) {
        MultiWifiJournalEntry *e = g_ptr_array_index(entries, i);
        if (e->op_type == MULTIWIFI_JOURNAL_OP_ADD_RULE) {
            GError *err = NULL;
            multiwifi_netlink_del_rule(nl_fd, e->table_id, e->rule_pref, e->fwmark, e->fwmask, e->src_ip, &err);
            if (err != NULL) {
                g_warning("Rollback rule del warning (pref %u): %s", e->rule_pref, err->message);
                g_error_free(err);
            }
        } else if (e->op_type == MULTIWIFI_JOURNAL_OP_ADD_ROUTE) {
            GError *err = NULL;
            multiwifi_netlink_del_route(nl_fd, e->table_id, e->gateway, e->ifname, &err);
            if (err != NULL) {
                g_warning("Rollback route del warning (table %u): %s", e->table_id, err->message);
                g_error_free(err);
            }
        }
    }

    g_info("Atomic rollback for operation '%s' completed.", multiwifi_route_journal_get_op_id(journal));
}

gboolean multiwifi_routing_manager_apply_plan(MultiWifiRoutingManager *self,
                                              const gchar *op_id,
                                              const MultiWifiRoutePlan *plan,
                                              MultiWifiOpManager *op_mgr,
                                              GError **error)
{
    if (self == NULL || op_id == NULL || plan == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to apply plan");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    /* Stage 1: VALIDATING */
    if (op_mgr != NULL) {
        multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_VALIDATING, 15, "Inspecting kernel tables and checking conflicts");
    }

    if (!multiwifi_routing_manager_check_conflicts(self, plan, error)) {
        if (op_mgr != NULL) {
            multiwifi_op_manager_finish(op_mgr, op_id, FALSE, NULL, "Validation failed: routing conflict detected");
        }
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd < 0) {
        if (op_mgr != NULL) {
            multiwifi_op_manager_finish(op_mgr, op_id, FALSE, NULL, "Netlink socket failure");
        }
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    /* Stage 2: APPLYING */
    if (op_mgr != NULL) {
        multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_APPLYING, 35, "Applying policy rules and routes");
    }

    MultiWifiRouteJournal *journal = multiwifi_route_journal_new(op_id);
    gchar *journal_filename = g_strdup_printf("%s.journal", op_id);
    gchar *journal_path = g_build_filename(self->journal_dir, journal_filename, NULL);
    g_free(journal_filename);

    gboolean apply_ok = TRUE;

    for (guint i = 0; i < plan->uplinks->len; i++) {
        MultiWifiUplinkRouteConfig *u = g_ptr_array_index(plan->uplinks, i);

        /* 1. Add source address IP rule */
        if (u->ipv4_address != NULL && *u->ipv4_address != '\0') {
            if (!multiwifi_netlink_add_rule(nl_fd, u->table_id, u->src_rule_pref, 0, 0, u->ipv4_address, error)) {
                apply_ok = FALSE;
                break;
            }
            multiwifi_route_journal_append(journal, multiwifi_journal_entry_new(
                MULTIWIFI_JOURNAL_OP_ADD_RULE, u->table_id, u->src_rule_pref, 0, 0, u->ipv4_address, NULL, u->ifname, 0
            ));
            multiwifi_route_journal_save_to_file(journal, journal_path, NULL);
        }

        /* 2. Add fwmark IP rule */
        if (u->fwmark > 0) {
            if (!multiwifi_netlink_add_rule(nl_fd, u->table_id, u->fwmark_rule_pref, u->fwmark, u->fwmask, NULL, error)) {
                apply_ok = FALSE;
                break;
            }
            multiwifi_route_journal_append(journal, multiwifi_journal_entry_new(
                MULTIWIFI_JOURNAL_OP_ADD_RULE, u->table_id, u->fwmark_rule_pref, u->fwmark, u->fwmask, NULL, NULL, u->ifname, 0
            ));
            multiwifi_route_journal_save_to_file(journal, journal_path, NULL);
        }

        /* 3. Add default route in designated table */
        if (!multiwifi_netlink_add_route(nl_fd, u->table_id, u->gateway, u->ifname, u->metric, error)) {
            apply_ok = FALSE;
            break;
        }
        multiwifi_route_journal_append(journal, multiwifi_journal_entry_new(
            MULTIWIFI_JOURNAL_OP_ADD_ROUTE, u->table_id, 0, 0, 0, NULL, u->gateway, u->ifname, u->metric
        ));
        multiwifi_route_journal_save_to_file(journal, journal_path, NULL);
    }

    if (!apply_ok) {
        if (op_mgr != NULL) {
            multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_ROLLING_BACK, 70, "Mutation failed; rolling back");
        }
        execute_journal_rollback(nl_fd, journal);
        multiwifi_route_journal_remove_file(journal_path);
        multiwifi_route_journal_free(journal);
        g_free(journal_path);
        multiwifi_netlink_close(nl_fd);
        if (op_mgr != NULL) {
            multiwifi_op_manager_finish(op_mgr, op_id, FALSE, NULL, "Transaction failed and cleanly rolled back");
        }
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    /* Stage 3: VERIFYING */
    if (op_mgr != NULL) {
        multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_VERIFYING, 85, "Verifying kernel RPDB and routing table state");
    }

    gboolean verify_ok = TRUE;
    for (guint i = 0; i < plan->uplinks->len; i++) {
        MultiWifiUplinkRouteConfig *u = g_ptr_array_index(plan->uplinks, i);

        if (u->ipv4_address != NULL && *u->ipv4_address != '\0') {
            gboolean rule_ok = FALSE;
            if (!multiwifi_netlink_verify_rule(nl_fd, u->table_id, u->src_rule_pref, &rule_ok, error) || !rule_ok) {
                if (error != NULL && *error == NULL) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Verification failed: Rule pref %u not found in kernel RPDB", u->src_rule_pref);
                }
                verify_ok = FALSE;
                break;
            }
        }

        if (u->fwmark > 0) {
            gboolean fw_rule_ok = FALSE;
            if (!multiwifi_netlink_verify_rule(nl_fd, u->table_id, u->fwmark_rule_pref, &fw_rule_ok, error) || !fw_rule_ok) {
                if (error != NULL && *error == NULL) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Verification failed: Fwmark rule pref %u not found in kernel RPDB", u->fwmark_rule_pref);
                }
                verify_ok = FALSE;
                break;
            }
        }

        gboolean route_ok = FALSE;
        if (!multiwifi_netlink_verify_route(nl_fd, u->table_id, u->gateway, u->ifname, &route_ok, error) || !route_ok) {
            if (error != NULL && *error == NULL) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Verification failed: Default route in table %u via %s dev %s missing",
                            u->table_id, u->gateway, u->ifname);
            }
            verify_ok = FALSE;
            break;
        }
    }

    if (!verify_ok) {
        if (op_mgr != NULL) {
            multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_ROLLING_BACK, 90, "Verification failed; rolling back");
        }
        execute_journal_rollback(nl_fd, journal);
        multiwifi_route_journal_remove_file(journal_path);
        multiwifi_route_journal_free(journal);
        g_free(journal_path);
        multiwifi_netlink_close(nl_fd);
        if (op_mgr != NULL) {
            multiwifi_op_manager_finish(op_mgr, op_id, FALSE, NULL, "Verification failed; changes rolled back");
        }
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    /* Stage 4: COMMIT */
    multiwifi_route_journal_remove_file(journal_path);
    multiwifi_route_journal_free(journal);
    g_free(journal_path);
    multiwifi_netlink_close(nl_fd);

    if (self->active_plan != NULL) {
        multiwifi_route_plan_free(self->active_plan);
    }

    /* Duplicate plan into active_plan */
    self->active_plan = multiwifi_route_plan_new();
    for (guint i = 0; i < plan->uplinks->len; i++) {
        MultiWifiUplinkRouteConfig *u = g_ptr_array_index(plan->uplinks, i);
        multiwifi_route_plan_add_uplink(self->active_plan, u->ifname, u->ifindex,
                                        u->ipv4_address, u->gateway, i);
    }
    self->is_active = TRUE;

    if (op_mgr != NULL) {
        multiwifi_op_manager_finish(op_mgr, op_id, TRUE, NULL, "Policy routing applied and verified successfully");
    }

    g_mutex_unlock(&self->mutex);
    return TRUE;
}

gboolean multiwifi_routing_manager_teardown(MultiWifiRoutingManager *self,
                                            const gchar *op_id,
                                            MultiWifiOpManager *op_mgr,
                                            GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Manager is NULL");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    if (self->active_plan == NULL || !self->is_active) {
        g_mutex_unlock(&self->mutex);
        if (op_mgr != NULL && op_id != NULL) {
            multiwifi_op_manager_finish(op_mgr, op_id, TRUE, NULL, "No active policy routing tables to teardown");
        }
        return TRUE;
    }

    if (op_mgr != NULL && op_id != NULL) {
        multiwifi_op_manager_update(op_mgr, op_id, MULTIWIFI_OP_STATE_APPLYING, 50, "Removing MultiWiFi routing rules and tables");
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd >= 0) {
        for (int i = (int)self->active_plan->uplinks->len - 1; i >= 0; i--) {
            MultiWifiUplinkRouteConfig *u = g_ptr_array_index(self->active_plan->uplinks, i);

            multiwifi_netlink_del_route(nl_fd, u->table_id, u->gateway, u->ifname, NULL);
            if (u->fwmark > 0) {
                multiwifi_netlink_del_rule(nl_fd, u->table_id, u->fwmark_rule_pref, u->fwmark, u->fwmask, NULL, NULL);
            }
            if (u->ipv4_address != NULL && *u->ipv4_address != '\0') {
                multiwifi_netlink_del_rule(nl_fd, u->table_id, u->src_rule_pref, 0, 0, u->ipv4_address, NULL);
            }
        }
        multiwifi_netlink_close(nl_fd);
    }

    multiwifi_route_plan_free(self->active_plan);
    self->active_plan = NULL;
    self->is_active = FALSE;

    if (op_mgr != NULL && op_id != NULL) {
        multiwifi_op_manager_finish(op_mgr, op_id, TRUE, NULL, "Policy routing tables cleanly dismantled");
    }

    g_mutex_unlock(&self->mutex);
    return TRUE;
}

gboolean multiwifi_routing_manager_recover(MultiWifiRoutingManager *self,
                                           GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Manager is NULL");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    if (!g_file_test(self->journal_dir, G_FILE_TEST_IS_DIR)) {
        g_mutex_unlock(&self->mutex);
        return TRUE;
    }

    GDir *dir = g_dir_open(self->journal_dir, 0, error);
    if (dir == NULL) {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    int nl_fd = multiwifi_netlink_open(error);
    if (nl_fd < 0) {
        g_dir_close(dir);
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    const gchar *entry_name = NULL;
    while ((entry_name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(entry_name, ".journal")) {
            gchar *full_path = g_build_filename(self->journal_dir, entry_name, NULL);
            g_info("Recovering orphaned journal: '%s'", full_path);

            MultiWifiRouteJournal *journal = multiwifi_route_journal_load_from_file(full_path, NULL);
            if (journal != NULL) {
                execute_journal_rollback(nl_fd, journal);
                multiwifi_route_journal_free(journal);
            }
            multiwifi_route_journal_remove_file(full_path);
            g_free(full_path);
        }
    }

    g_dir_close(dir);
    multiwifi_netlink_close(nl_fd);
    g_mutex_unlock(&self->mutex);
    return TRUE;
}

gboolean multiwifi_routing_manager_is_active(MultiWifiRoutingManager *self)
{
    g_mutex_lock(&self->mutex);
    gboolean active = self ? self->is_active : FALSE;
    g_mutex_unlock(&self->mutex);
    return active;
}
