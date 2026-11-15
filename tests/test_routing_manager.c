#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "multiwifi/core/journal.h"
#include "multiwifi/core/netlink_route.h"
#include "multiwifi/core/routing_manager.h"

static gboolean enter_isolated_network_namespace(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        return FALSE;
    }

    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        const char *deny = "deny";
        if (write(fd, deny, strlen(deny)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    char map[64];
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", gid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", uid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    /* Bring lo up and assign test IPv4 address */
    int res = system("ip link set lo up && ip addr add 192.168.10.2/24 dev lo");
    return (res == 0);
}

static void test_route_plan_lifecycle(void)
{
    MultiWifiRoutePlan *plan = multiwifi_route_plan_new();
    g_assert_nonnull(plan);

    multiwifi_route_plan_add_uplink(plan, "wlp0s20f3", 2, "192.168.1.50", "192.168.1.1", 0);
    multiwifi_route_plan_add_uplink(plan, "wlp0s20f0u1", 3, "192.168.2.50", "192.168.2.1", 1);

    g_assert_cmpuint(plan->uplinks->len, ==, 2);

    MultiWifiUplinkRouteConfig *u0 = g_ptr_array_index(plan->uplinks, 0);
    g_assert_cmpstr(u0->ifname, ==, "wlp0s20f3");
    g_assert_cmpuint(u0->ifindex, ==, 2);
    g_assert_cmpstr(u0->ipv4_address, ==, "192.168.1.50");
    g_assert_cmpstr(u0->gateway, ==, "192.168.1.1");
    g_assert_cmpuint(u0->table_id, ==, 10001);
    g_assert_cmpuint(u0->src_rule_pref, ==, 20001);
    g_assert_cmpuint(u0->fwmark_rule_pref, ==, 21001);
    g_assert_cmpuint(u0->fwmark, ==, 0x1001);
    g_assert_cmpuint(u0->fwmask, ==, 0xff00);

    MultiWifiUplinkRouteConfig *u1 = g_ptr_array_index(plan->uplinks, 1);
    g_assert_cmpstr(u1->ifname, ==, "wlp0s20f0u1");
    g_assert_cmpuint(u1->table_id, ==, 10002);
    g_assert_cmpuint(u1->src_rule_pref, ==, 20002);
    g_assert_cmpuint(u1->fwmark_rule_pref, ==, 21002);
    g_assert_cmpuint(u1->fwmark, ==, 0x1002);

    multiwifi_route_plan_free(plan);
}

static void test_route_journal_file_persistence(void)
{
    gchar *tmp_file = g_build_filename(g_get_tmp_dir(), "test_journal_rw.ini", NULL);
    multiwifi_route_journal_remove_file(tmp_file);

    MultiWifiRouteJournal *j1 = multiwifi_route_journal_new("test-op-1234");
    g_assert_nonnull(j1);
    g_assert_cmpstr(multiwifi_route_journal_get_op_id(j1), ==, "test-op-1234");

    multiwifi_route_journal_append(j1, multiwifi_journal_entry_new(
        MULTIWIFI_JOURNAL_OP_ADD_RULE, 10001, 20001, 0, 0, "192.168.1.100", NULL, "wlan0", 0
    ));
    multiwifi_route_journal_append(j1, multiwifi_journal_entry_new(
        MULTIWIFI_JOURNAL_OP_ADD_ROUTE, 10001, 0, 0, 0, NULL, "192.168.1.1", "wlan0", 100
    ));

    GError *error = NULL;
    gboolean saved = multiwifi_route_journal_save_to_file(j1, tmp_file, &error);
    g_assert_no_error(error);
    g_assert_true(saved);
    multiwifi_route_journal_free(j1);

    /* Load back and verify */
    MultiWifiRouteJournal *j2 = multiwifi_route_journal_load_from_file(tmp_file, &error);
    g_assert_no_error(error);
    g_assert_nonnull(j2);
    g_assert_cmpstr(multiwifi_route_journal_get_op_id(j2), ==, "test-op-1234");

    GPtrArray *entries = multiwifi_route_journal_get_entries(j2);
    g_assert_nonnull(entries);
    g_assert_cmpuint(entries->len, ==, 2);

    MultiWifiJournalEntry *e0 = g_ptr_array_index(entries, 0);
    g_assert_cmpint(e0->op_type, ==, MULTIWIFI_JOURNAL_OP_ADD_RULE);
    g_assert_cmpuint(e0->table_id, ==, 10001);
    g_assert_cmpuint(e0->rule_pref, ==, 20001);
    g_assert_cmpstr(e0->src_ip, ==, "192.168.1.100");

    MultiWifiJournalEntry *e1 = g_ptr_array_index(entries, 1);
    g_assert_cmpint(e1->op_type, ==, MULTIWIFI_JOURNAL_OP_ADD_ROUTE);
    g_assert_cmpuint(e1->table_id, ==, 10001);
    g_assert_cmpstr(e1->gateway, ==, "192.168.1.1");
    g_assert_cmpstr(e1->ifname, ==, "wlan0");
    g_assert_cmpuint(e1->metric, ==, 100);

    multiwifi_route_journal_free(j2);
    multiwifi_route_journal_remove_file(tmp_file);
    g_free(tmp_file);
}

static void test_conflict_detection_validation(void)
{
    MultiWifiRoutingManager *mgr = multiwifi_routing_manager_new(NULL);
    g_assert_nonnull(mgr);

    GError *error = NULL;

    /* 1. Empty plan */
    MultiWifiRoutePlan *empty_plan = multiwifi_route_plan_new();
    gboolean safe = multiwifi_routing_manager_check_conflicts(mgr, empty_plan, &error);
    g_assert_false(safe);
    g_assert_nonnull(error);
    g_clear_error(&error);
    multiwifi_route_plan_free(empty_plan);

    /* 2. Plan with invalid gateway */
    MultiWifiRoutePlan *bad_gw_plan = multiwifi_route_plan_new();
    multiwifi_route_plan_add_uplink(bad_gw_plan, "lo", 1, "127.0.0.1", "not-an-ip", 0);
    safe = multiwifi_routing_manager_check_conflicts(mgr, bad_gw_plan, &error);
    g_assert_false(safe);
    g_assert_nonnull(error);
    g_clear_error(&error);
    multiwifi_route_plan_free(bad_gw_plan);

    /* 3. Plan with reserved table ID (<= 255) */
    MultiWifiRoutePlan *reserved_tbl_plan = multiwifi_route_plan_new();
    multiwifi_route_plan_add_uplink(reserved_tbl_plan, "lo", 1, "127.0.0.1", "127.0.0.1", 0);
    MultiWifiUplinkRouteConfig *u = g_ptr_array_index(reserved_tbl_plan->uplinks, 0);
    u->table_id = 254; /* main table */
    safe = multiwifi_routing_manager_check_conflicts(mgr, reserved_tbl_plan, &error);
    g_assert_false(safe);
    g_assert_nonnull(error);
    g_clear_error(&error);
    multiwifi_route_plan_free(reserved_tbl_plan);

    g_object_unref(mgr);
}

static void test_netlink_in_network_namespace(void)
{
    if (!enter_isolated_network_namespace()) {
        g_test_skip("Kernel unprivileged user namespaces disabled on host; skipping namespace tests");
        return;
    }

    GError *error = NULL;
    int nl_fd = multiwifi_netlink_open(&error);
    g_assert_no_error(error);
    g_assert_cmpint(nl_fd, >=, 0);

    /* Add IP rule */
    gboolean ok = multiwifi_netlink_add_rule(nl_fd, 10001, 20001, 0, 0, "192.168.10.2", &error);
    g_assert_no_error(error);
    g_assert_true(ok);

    /* Verify IP rule exists */
    gboolean exists = FALSE;
    ok = multiwifi_netlink_verify_rule(nl_fd, 10001, 20001, &exists, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_true(exists);

    /* Add route in table 10001 */
    ok = multiwifi_netlink_add_route(nl_fd, 10001, "192.168.10.1", "lo", 100, &error);
    g_assert_no_error(error);
    g_assert_true(ok);

    /* Verify route exists */
    gboolean route_exists = FALSE;
    ok = multiwifi_netlink_verify_route(nl_fd, 10001, "192.168.10.1", "lo", &route_exists, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_true(route_exists);

    /* Delete route */
    ok = multiwifi_netlink_del_route(nl_fd, 10001, "192.168.10.1", "lo", &error);
    g_assert_no_error(error);
    g_assert_true(ok);

    route_exists = TRUE;
    multiwifi_netlink_verify_route(nl_fd, 10001, "192.168.10.1", "lo", &route_exists, NULL);
    g_assert_false(route_exists);

    /* Delete rule */
    ok = multiwifi_netlink_del_rule(nl_fd, 10001, 20001, 0, 0, "192.168.10.2", &error);
    g_assert_no_error(error);
    g_assert_true(ok);

    exists = TRUE;
    multiwifi_netlink_verify_rule(nl_fd, 10001, 20001, &exists, NULL);
    g_assert_false(exists);

    multiwifi_netlink_close(nl_fd);
}

static void test_transaction_apply_verify_teardown(void)
{
    if (!enter_isolated_network_namespace()) {
        g_test_skip("Kernel unprivileged user namespaces disabled on host; skipping namespace tests");
        return;
    }

    gchar *tmp_journal_dir = g_build_filename(g_get_tmp_dir(), "multiwifi_test_journals", NULL);
    MultiWifiRoutingManager *mgr = multiwifi_routing_manager_new(tmp_journal_dir);
    MultiWifiOpManager *op_mgr = multiwifi_op_manager_new();

    MultiWifiRoutePlan *plan = multiwifi_route_plan_new();
    multiwifi_route_plan_add_uplink(plan, "lo", 1, "192.168.10.2", "192.168.10.1", 0);

    gchar *op_id = multiwifi_op_manager_create_op(op_mgr, "ApplyTest");

    GError *error = NULL;
    gboolean applied = multiwifi_routing_manager_apply_plan(mgr, op_id, plan, op_mgr, &error);
    g_assert_no_error(error);
    g_assert_true(applied);
    g_assert_true(multiwifi_routing_manager_is_active(mgr));
    g_assert_cmpuint(multiwifi_routing_manager_get_fwmark(mgr, "lo"), ==, 0x1001);
    g_assert_cmpuint(multiwifi_routing_manager_get_fwmark(mgr, "nonexistent0"), ==, 0);
    g_assert_cmpuint(multiwifi_routing_manager_get_fwmark(NULL, "lo"), ==, 0);

    MultiWifiOperation *op = multiwifi_op_manager_find(op_mgr, op_id);
    g_assert_nonnull(op);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_SUCCEEDED);
    g_assert_cmpuint(op->percent, ==, 100);
    g_assert_true(op->success);
    multiwifi_operation_free(op);

    /* Verify in kernel */
    int nl_fd = multiwifi_netlink_open(NULL);
    g_assert_cmpint(nl_fd, >=, 0);

    gboolean rule_ok = FALSE;
    multiwifi_netlink_verify_rule(nl_fd, 10001, 20001, &rule_ok, NULL);
    g_assert_true(rule_ok);

    gboolean route_ok = FALSE;
    multiwifi_netlink_verify_route(nl_fd, 10001, "192.168.10.1", "lo", &route_ok, NULL);
    g_assert_true(route_ok);

    /* Teardown */
    gchar *td_op_id = multiwifi_op_manager_create_op(op_mgr, "TeardownTest");
    gboolean torn_down = multiwifi_routing_manager_teardown(mgr, td_op_id, op_mgr, &error);
    g_assert_no_error(error);
    g_assert_true(torn_down);
    g_assert_false(multiwifi_routing_manager_is_active(mgr));
    g_assert_cmpuint(multiwifi_routing_manager_get_fwmark(mgr, "lo"), ==, 0);

    /* Verify deleted from kernel */
    rule_ok = TRUE;
    multiwifi_netlink_verify_rule(nl_fd, 10001, 20001, &rule_ok, NULL);
    g_assert_false(rule_ok);

    route_ok = TRUE;
    multiwifi_netlink_verify_route(nl_fd, 10001, "192.168.10.1", "lo", &route_ok, NULL);
    g_assert_false(route_ok);

    multiwifi_netlink_close(nl_fd);
    multiwifi_route_plan_free(plan);
    g_free(op_id);
    g_free(td_op_id);
    g_object_unref(op_mgr);
    g_object_unref(mgr);
    g_free(tmp_journal_dir);
}

static void test_transaction_failure_and_rollback(void)
{
    if (!enter_isolated_network_namespace()) {
        g_test_skip("Kernel unprivileged user namespaces disabled on host; skipping namespace tests");
        return;
    }

    gchar *tmp_journal_dir = g_build_filename(g_get_tmp_dir(), "multiwifi_test_journals_fail", NULL);
    MultiWifiRoutingManager *mgr = multiwifi_routing_manager_new(tmp_journal_dir);
    MultiWifiOpManager *op_mgr = multiwifi_op_manager_new();

    /* Plan with 1 valid uplink and 1 invalid uplink (non-existent device) */
    MultiWifiRoutePlan *plan = multiwifi_route_plan_new();
    multiwifi_route_plan_add_uplink(plan, "lo", 1, "192.168.10.2", "192.168.10.1", 0);
    multiwifi_route_plan_add_uplink(plan, "nonexistent_dev", 99, "192.168.20.2", "192.168.20.1", 1);

    gchar *op_id = multiwifi_op_manager_create_op(op_mgr, "FailApplyTest");

    GError *error = NULL;
    gboolean applied = multiwifi_routing_manager_apply_plan(mgr, op_id, plan, op_mgr, &error);
    g_assert_false(applied);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(multiwifi_routing_manager_is_active(mgr));

    MultiWifiOperation *op = multiwifi_op_manager_find(op_mgr, op_id);
    g_assert_nonnull(op);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_FAILED);
    g_assert_false(op->success);
    multiwifi_operation_free(op);

    /* Verify that rollback cleanly removed the first uplink's rule and route! */
    int nl_fd = multiwifi_netlink_open(NULL);
    g_assert_cmpint(nl_fd, >=, 0);

    gboolean rule_ok = TRUE;
    multiwifi_netlink_verify_rule(nl_fd, 10001, 20001, &rule_ok, NULL);
    g_assert_false(rule_ok);

    gboolean route_ok = TRUE;
    multiwifi_netlink_verify_route(nl_fd, 10001, "192.168.10.1", "lo", &route_ok, NULL);
    g_assert_false(route_ok);

    multiwifi_netlink_close(nl_fd);
    multiwifi_route_plan_free(plan);
    g_free(op_id);
    g_object_unref(op_mgr);
    g_object_unref(mgr);
    g_free(tmp_journal_dir);
}

static void test_crash_recovery_reconciliation(void)
{
    if (!enter_isolated_network_namespace()) {
        g_test_skip("Kernel unprivileged user namespaces disabled on host; skipping namespace tests");
        return;
    }

    gchar *tmp_journal_dir = g_build_filename(g_get_tmp_dir(), "multiwifi_recovery_test", NULL);
    g_mkdir_with_parents(tmp_journal_dir, 0700);

    /* 1. Manually add an orphan rule and route as if left by a crashed daemon */
    int nl_fd = multiwifi_netlink_open(NULL);
    g_assert_cmpint(nl_fd, >=, 0);

    multiwifi_netlink_add_rule(nl_fd, 10005, 20005, 0, 0, "192.168.10.2", NULL);
    multiwifi_netlink_add_route(nl_fd, 10005, "192.168.10.1", "lo", 100, NULL);

    gboolean rule_ok = FALSE;
    multiwifi_netlink_verify_rule(nl_fd, 10005, 20005, &rule_ok, NULL);
    g_assert_true(rule_ok);

    multiwifi_netlink_close(nl_fd);

    /* 2. Write an abandoned .journal file */
    MultiWifiRouteJournal *journal = multiwifi_route_journal_new("crashed-op-999");
    multiwifi_route_journal_append(journal, multiwifi_journal_entry_new(
        MULTIWIFI_JOURNAL_OP_ADD_RULE, 10005, 20005, 0, 0, "192.168.10.2", NULL, "lo", 0
    ));
    multiwifi_route_journal_append(journal, multiwifi_journal_entry_new(
        MULTIWIFI_JOURNAL_OP_ADD_ROUTE, 10005, 0, 0, 0, NULL, "192.168.10.1", "lo", 100
    ));

    gchar *journal_path = g_build_filename(tmp_journal_dir, "crashed-op-999.journal", NULL);
    multiwifi_route_journal_save_to_file(journal, journal_path, NULL);
    multiwifi_route_journal_free(journal);

    /* 3. Instantiate RoutingManager and trigger recovery */
    MultiWifiRoutingManager *mgr = multiwifi_routing_manager_new(tmp_journal_dir);
    GError *error = NULL;
    gboolean recovered = multiwifi_routing_manager_recover(mgr, &error);
    g_assert_no_error(error);
    g_assert_true(recovered);

    /* 4. Verify orphaned rule & route were cleanly removed and journal file deleted */
    nl_fd = multiwifi_netlink_open(NULL);
    rule_ok = TRUE;
    multiwifi_netlink_verify_rule(nl_fd, 10005, 20005, &rule_ok, NULL);
    g_assert_false(rule_ok);

    gboolean route_ok = TRUE;
    multiwifi_netlink_verify_route(nl_fd, 10005, "192.168.10.1", "lo", &route_ok, NULL);
    g_assert_false(route_ok);

    multiwifi_netlink_close(nl_fd);
    g_assert_false(g_file_test(journal_path, G_FILE_TEST_EXISTS));

    g_free(journal_path);
    g_object_unref(mgr);
    g_rmdir(tmp_journal_dir);
    g_free(tmp_journal_dir);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/routing/plan_lifecycle", test_route_plan_lifecycle);
    g_test_add_func("/routing/journal_file_persistence", test_route_journal_file_persistence);
    g_test_add_func("/routing/conflict_detection_validation", test_conflict_detection_validation);
    g_test_add_func("/routing/netlink_in_network_namespace", test_netlink_in_network_namespace);
    g_test_add_func("/routing/transaction_apply_verify_teardown", test_transaction_apply_verify_teardown);
    g_test_add_func("/routing/transaction_failure_and_rollback", test_transaction_failure_and_rollback);
    g_test_add_func("/routing/crash_recovery_reconciliation", test_crash_recovery_reconciliation);

    return g_test_run();
}
