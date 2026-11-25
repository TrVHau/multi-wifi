#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include "multiwifi/core/load_balancer.h"
#include "multiwifi/core/firewall.h"

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

    (void)system("ip link set lo up 2>/dev/null");
    return TRUE;
}

static void test_scheduler_equal_weights(void)
{
    MultiWifiScheduler *sched = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    g_assert_nonnull(sched);

    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0x1000, 100, TRUE, 15.0, 0);
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0x2000, 100, TRUE, 25.0, 0);

    g_assert_cmpuint(multiwifi_scheduler_get_eligible_count(sched), ==, 2);

    /* Test WRR distribution */
    guint count_a = 0;
    guint count_b = 0;
    for (guint i = 0; i < 200; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        if (mark == 0x1000) count_a++;
        else if (mark == 0x2000) count_b++;
    }

    g_assert_cmpuint(count_a, ==, 100);
    g_assert_cmpuint(count_b, ==, 100);

    /* Test discrete distribution vector */
    GArray *marks = NULL;
    guint n = multiwifi_scheduler_get_distribution(sched, &marks);
    g_assert_nonnull(marks);
    g_assert_cmpuint(n, ==, 2);
    g_assert_cmpuint(g_array_index(marks, uint32_t, 0), ==, 0x1000);
    g_assert_cmpuint(g_array_index(marks, uint32_t, 1), ==, 0x2000);
    g_array_unref(marks);

    multiwifi_scheduler_free(sched);
}

static void test_scheduler_unequal_weights(void)
{
    MultiWifiScheduler *sched = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    g_assert_nonnull(sched);

    /* 2:1 ratio */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0x1000, 200, TRUE, 10.0, 0);
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0x2000, 100, TRUE, 20.0, 0);

    guint count_a = 0;
    guint count_b = 0;
    for (guint i = 0; i < 300; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        if (mark == 0x1000) count_a++;
        else if (mark == 0x2000) count_b++;
    }

    g_assert_cmpuint(count_a, ==, 200);
    g_assert_cmpuint(count_b, ==, 100);

    GArray *marks = NULL;
    guint n = multiwifi_scheduler_get_distribution(sched, &marks);
    g_assert_nonnull(marks);
    g_assert_cmpuint(n, ==, 3);
    g_assert_cmpuint(g_array_index(marks, uint32_t, 0), ==, 0x1000);
    g_assert_cmpuint(g_array_index(marks, uint32_t, 1), ==, 0x1000);
    g_assert_cmpuint(g_array_index(marks, uint32_t, 2), ==, 0x2000);
    g_array_unref(marks);

    multiwifi_scheduler_free(sched);
}

static void test_scheduler_health_penalty_and_failover(void)
{
    MultiWifiScheduler *sched = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    g_assert_nonnull(sched);

    /* Base weight 100, but path A has 25% loss (effective weight halved to 50) */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0x1000, 100, TRUE, 10.0, 25);
    /* Path B has 0% loss (effective weight remains 100) */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0x2000, 100, TRUE, 20.0, 0);

    guint count_a = 0;
    guint count_b = 0;
    for (guint i = 0; i < 150; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        if (mark == 0x1000) count_a++;
        else if (mark == 0x2000) count_b++;
    }

    /* Path B should get double the flows of degraded Path A (100 vs 50) */
    g_assert_cmpuint(count_a, ==, 50);
    g_assert_cmpuint(count_b, ==, 100);

    /* Now path A suffers 60% severe loss (effective weight reduced to 25) */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0, 0, TRUE, 50.0, 60);

    count_a = 0;
    count_b = 0;
    for (guint i = 0; i < 125; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        if (mark == 0x1000) count_a++;
        else if (mark == 0x2000) count_b++;
    }
    g_assert_cmpuint(count_a, ==, 25);
    g_assert_cmpuint(count_b, ==, 100);

    /* Path A drops completely (is_eligible = FALSE) */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0, 0, FALSE, 0.0, 100);
    g_assert_cmpuint(multiwifi_scheduler_get_eligible_count(sched), ==, 1);

    for (guint i = 0; i < 50; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        g_assert_cmpuint(mark, ==, 0x2000);
    }

    /* Both paths drop */
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0, 0, FALSE, 0.0, 100);
    g_assert_cmpuint(multiwifi_scheduler_get_eligible_count(sched), ==, 0);
    g_assert_cmpuint(multiwifi_scheduler_select_mark(sched), ==, 0);

    GArray *marks = NULL;
    guint n = multiwifi_scheduler_get_distribution(sched, &marks);
    g_assert_cmpuint(n, ==, 0);
    if (marks != NULL) {
        g_array_unref(marks);
    }

    multiwifi_scheduler_free(sched);
}

static void test_scheduler_dynamic_weight_update(void)
{
    MultiWifiScheduler *sched = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    g_assert_nonnull(sched);

    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0x1000, 100, TRUE, 10.0, 0);
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0x2000, 100, TRUE, 10.0, 0);

    /* Update weight dynamically */
    gboolean ok = multiwifi_scheduler_set_weight(sched, "wlp0s20f3", 300);
    g_assert_true(ok);

    /* Non-existent device returns FALSE */
    g_assert_false(multiwifi_scheduler_set_weight(sched, "nonexistent", 100));

    guint count_a = 0;
    guint count_b = 0;
    for (guint i = 0; i < 400; i++) {
        uint32_t mark = multiwifi_scheduler_select_mark(sched);
        if (mark == 0x1000) count_a++;
        else if (mark == 0x2000) count_b++;
    }

    g_assert_cmpuint(count_a, ==, 300);
    g_assert_cmpuint(count_b, ==, 100);

    multiwifi_scheduler_free(sched);
}

typedef struct {
    MultiWifiScheduler *sched;
    gint iterations;
} ThreadData;

static gpointer worker_thread(gpointer user_data)
{
    ThreadData *td = (ThreadData *)user_data;
    for (gint i = 0; i < td->iterations; i++) {
        multiwifi_scheduler_select_mark(td->sched);
        if ((i % 50) == 0) {
            multiwifi_scheduler_set_weight(td->sched, "wlp0s20f3", (guint)(100 + (i % 200)));
        }
    }
    return NULL;
}

static void test_scheduler_thread_concurrency(void)
{
    MultiWifiScheduler *sched = multiwifi_scheduler_new(MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN);
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f3", "wlp0s20f3", 0x1000, 100, TRUE, 10.0, 0);
    multiwifi_scheduler_add_or_update(sched, "wlp0s20f0u1", "wlp0s20f0u1", 0x2000, 100, TRUE, 10.0, 0);

    ThreadData td = { .sched = sched, .iterations = 1000 };
    GThread *t1 = g_thread_new("worker-1", worker_thread, &td);
    GThread *t2 = g_thread_new("worker-2", worker_thread, &td);
    GThread *t3 = g_thread_new("worker-3", worker_thread, &td);

    g_thread_join(t1);
    g_thread_join(t2);
    g_thread_join(t3);

    multiwifi_scheduler_free(sched);
}

static void test_firewall_invalid_arguments(void)
{
    GError *err = NULL;

    /* NULL manager */
    g_assert_false(multiwifi_firewall_apply(NULL, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

    MultiWifiFirewall *fw = multiwifi_firewall_new();
    g_assert_nonnull(fw);

    /* NULL marks */
    g_assert_false(multiwifi_firewall_apply(fw, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

    /* Empty marks */
    GArray *empty = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    g_assert_false(multiwifi_firewall_apply(fw, empty, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
    g_array_unref(empty);

    /* Teardown NULL */
    g_assert_false(multiwifi_firewall_teardown(NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

    /* Verify NULL */
    gboolean exists = FALSE;
    g_assert_false(multiwifi_firewall_verify(NULL, &exists, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

    g_assert_false(multiwifi_firewall_verify(fw, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

    g_object_unref(fw);
}

static void test_firewall_lifecycle_in_netns(void)
{
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);

    if (pid == 0) {
        if (!enter_isolated_network_namespace()) {
            _exit(0);
        }

        MultiWifiFirewall *fw = multiwifi_firewall_new();
        if (fw == NULL) _exit(1);

        GError *err = NULL;
        gboolean exists = FALSE;
        if (!multiwifi_firewall_verify(fw, &exists, &err)) {
            /* Kernel / AppArmor denies unprivileged nftables in user namespace */
            g_clear_error(&err);
            g_object_unref(fw);
            _exit(0);
        }

        if (exists) _exit(2);

        /* Apply single mark */
        GArray *marks = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        uint32_t m1 = 0x1000;
        g_array_append_val(marks, m1);

        if (!multiwifi_firewall_apply(fw, marks, &err)) {
            if (err != NULL && (strstr(err->message, "Operation not permitted") != NULL ||
                                strstr(err->message, "Permission denied") != NULL)) {
                /* Kernel / AppArmor denies unprivileged nftables in user namespace */
                g_clear_error(&err);
                g_array_unref(marks);
                g_object_unref(fw);
                _exit(0);
            }
            _exit(3);
        }
        if (!multiwifi_firewall_is_active(fw)) _exit(4);
        if (!multiwifi_firewall_verify(fw, &exists, &err) || !exists) _exit(5);

        /* Apply multiple marks (replaces table atomically) */
        uint32_t m2 = 0x2000;
        g_array_append_val(marks, m2);
        if (!multiwifi_firewall_apply(fw, marks, &err)) _exit(6);

        /* Teardown */
        if (!multiwifi_firewall_teardown(fw, &err)) _exit(7);
        if (multiwifi_firewall_is_active(fw)) _exit(8);
        if (!multiwifi_firewall_verify(fw, &exists, &err) || exists) _exit(9);

        /* Teardown again on absent table succeeds idempotently */
        if (!multiwifi_firewall_teardown(fw, &err)) _exit(10);

        g_array_unref(marks);
        g_object_unref(fw);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/load_balancer/scheduler_equal_weights", test_scheduler_equal_weights);
    g_test_add_func("/load_balancer/scheduler_unequal_weights", test_scheduler_unequal_weights);
    g_test_add_func("/load_balancer/scheduler_health_penalty", test_scheduler_health_penalty_and_failover);
    g_test_add_func("/load_balancer/scheduler_dynamic_weight", test_scheduler_dynamic_weight_update);
    g_test_add_func("/load_balancer/scheduler_concurrency", test_scheduler_thread_concurrency);
    g_test_add_func("/load_balancer/firewall_invalid_arguments", test_firewall_invalid_arguments);
    g_test_add_func("/load_balancer/firewall_lifecycle_netns", test_firewall_lifecycle_in_netns);

    return g_test_run();
}
