#include "multiwifi/core/path_monitor.h"
#include <glib.h>

typedef struct {
    gboolean return_success;
    gdouble return_rtt;
    guint call_count;
} MockProbeContext;

static gboolean mock_probe(const gchar *ifname,
                           const gchar *source_ip,
                           const gchar *target_ip,
                           gdouble *out_rtt_ms,
                           gpointer user_data)
{
    (void)ifname;
    (void)source_ip;
    (void)target_ip;
    MockProbeContext *ctx = (MockProbeContext *)user_data;
    ctx->call_count++;
    if (out_rtt_ms != NULL) {
        *out_rtt_ms = ctx->return_rtt;
    }
    return ctx->return_success;
}

static void test_path_info_lifecycle(void)
{
    MultiWifiPathInfo *info = multiwifi_path_info_new("wlp0s20f0u1");
    g_assert_nonnull(info);
    g_assert_cmpstr(info->ifname, ==, "wlp0s20f0u1");
    g_assert_cmpint(info->state, ==, MULTIWIFI_PATH_STATE_UNKNOWN);
    g_assert_false(info->is_eligible);

    info->gateway = g_strdup("192.168.1.1");
    info->source_ip = g_strdup("192.168.1.50/24");
    info->state = MULTIWIFI_PATH_STATE_ONLINE;
    info->rtt_ms = 18.5;
    info->is_eligible = TRUE;

    MultiWifiPathInfo *copy = multiwifi_path_info_copy(info);
    g_assert_nonnull(copy);
    g_assert_cmpstr(copy->ifname, ==, info->ifname);
    g_assert_cmpstr(copy->gateway, ==, info->gateway);
    g_assert_cmpstr(copy->source_ip, ==, info->source_ip);
    g_assert_cmpint(copy->state, ==, info->state);
    g_assert_true(copy->is_eligible);

    g_assert_cmpstr(multiwifi_path_state_to_string(MULTIWIFI_PATH_STATE_ONLINE), ==, "Online");
    g_assert_cmpstr(multiwifi_path_state_to_string(MULTIWIFI_PATH_STATE_OFFLINE), ==, "Offline");

    multiwifi_path_info_free(copy);
    multiwifi_path_info_free(info);
}

static void test_hysteresis_recovery_and_failure(void)
{
    MultiWifiPathMonitor *mon = multiwifi_path_monitor_new();
    MockProbeContext mock_ctx = {
        .return_success = TRUE,
        .return_rtt = 15.0,
        .call_count = 0
    };
    multiwifi_path_monitor_set_probe_func(mon, mock_probe, &mock_ctx);

    /* Configure thresholds: 3 failures to drop, 3 successes to recover */
    MultiWifiLoadBalanceConfig cfg = {
        .strategy = MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN,
        .health_check_interval_ms = 1000,
        .failure_threshold = 3,
        .recovery_threshold = 3,
        .cooldown_ms = 0 /* 0 for deterministic test */
    };
    multiwifi_path_monitor_set_config(mon, &cfg);

    MultiWifiDeviceInfo *dev = multiwifi_device_info_new();
    dev->ifname = g_strdup("wlan0");
    dev->state = MULTIWIFI_DEVICE_STATE_CONNECTED;
    dev->ipv4_address = g_strdup("192.168.1.100/24");
    dev->ipv4_gateway = g_strdup("192.168.1.1");

    multiwifi_path_monitor_update_device(mon, dev);

    /* Check initial state: PROBING */
    MultiWifiPathInfo *p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_nonnull(p);
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_PROBING);
    g_assert_false(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Probe 1 (success): streak = 1, should NOT be online yet */
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_PROBING);
    g_assert_cmpuint(p->consecutive_successes, ==, 1);
    g_assert_false(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Probe 2 (success): streak = 2, should NOT be online yet */
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_PROBING);
    g_assert_cmpuint(p->consecutive_successes, ==, 2);
    g_assert_false(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Probe 3 (success): streak = 3 == recovery_threshold -> MUST BE ONLINE */
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_ONLINE);
    g_assert_cmpuint(p->consecutive_successes, ==, 3);
    g_assert_true(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Now simulate failure: Probe 4 (fail) */
    mock_ctx.return_success = FALSE;
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    /* Hysteresis: 1 failure should keep it ONLINE */
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_ONLINE);
    g_assert_cmpuint(p->consecutive_failures, ==, 1);
    g_assert_true(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Probe 5 (fail): 2nd failure, still holds ONLINE */
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_ONLINE);
    g_assert_cmpuint(p->consecutive_failures, ==, 2);
    multiwifi_path_info_free(p);

    /* Probe 6 (fail): 3rd failure == failure_threshold -> transitions to LOCAL_ONLY / un-eligible */
    multiwifi_path_monitor_probe_all_now(mon);
    p = multiwifi_path_monitor_find_path(mon, "wlan0");
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_LOCAL_ONLY);
    g_assert_cmpuint(p->consecutive_failures, ==, 3);
    g_assert_false(p->is_eligible);
    multiwifi_path_info_free(p);

    multiwifi_device_info_free(dev);
    g_object_unref(mon);
}

static void test_device_disconnect_event(void)
{
    MultiWifiPathMonitor *mon = multiwifi_path_monitor_new();
    MultiWifiDeviceInfo *dev = multiwifi_device_info_new();
    dev->ifname = g_strdup("wlan1");
    dev->state = MULTIWIFI_DEVICE_STATE_CONNECTED;
    dev->ipv4_address = g_strdup("10.0.0.5/24");
    dev->ipv4_gateway = g_strdup("10.0.0.1");

    multiwifi_path_monitor_update_device(mon, dev);

    /* Now device disconnects */
    dev->state = MULTIWIFI_DEVICE_STATE_DISCONNECTED;
    multiwifi_path_monitor_update_device(mon, dev);

    MultiWifiPathInfo *p = multiwifi_path_monitor_find_path(mon, "wlan1");
    g_assert_nonnull(p);
    g_assert_cmpint(p->state, ==, MULTIWIFI_PATH_STATE_OFFLINE);
    g_assert_false(p->is_eligible);
    multiwifi_path_info_free(p);

    /* Device removed entirely */
    multiwifi_path_monitor_remove_device(mon, "wlan1");
    p = multiwifi_path_monitor_find_path(mon, "wlan1");
    g_assert_null(p);

    multiwifi_device_info_free(dev);
    g_object_unref(mon);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/path_monitor/info_lifecycle", test_path_info_lifecycle);
    g_test_add_func("/path_monitor/hysteresis_recovery_and_failure", test_hysteresis_recovery_and_failure);
    g_test_add_func("/path_monitor/device_disconnect_event", test_device_disconnect_event);

    return g_test_run();
}
