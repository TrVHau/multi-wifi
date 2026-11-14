#include "multiwifi/core/wifi_device.h"
#include "multiwifi/core/wifi_inventory.h"
#include <glib.h>

static void test_device_info_lifecycle(void)
{
    MultiWifiDeviceInfo *dev = multiwifi_device_info_new();
    g_assert_nonnull(dev);
    g_assert_cmpint(dev->ifindex, ==, -1);
    g_assert_cmpint(dev->state, ==, MULTIWIFI_DEVICE_STATE_UNKNOWN);
    g_assert_null(dev->ifname);
    g_assert_null(dev->ssid);

    dev->ifname = g_strdup("wlp0s20f0u1");
    dev->ifindex = 4;
    dev->hw_address = g_strdup("50:2b:73:d8:23:6b");
    dev->state = MULTIWIFI_DEVICE_STATE_CONNECTED;
    g_free(dev->state_str);
    dev->state_str = g_strdup("Connected");
    dev->ssid = g_strdup("PTIT_WIFI");
    dev->signal_strength = 82;
    dev->frequency = 2437;
    dev->ipv4_address = g_strdup("10.10.20.15/24");
    dev->ipv4_gateway = g_strdup("10.10.20.1");

    /* Deep copy verification */
    MultiWifiDeviceInfo *copy = multiwifi_device_info_copy(dev);
    g_assert_nonnull(copy);
    g_assert_cmpstr(copy->ifname, ==, dev->ifname);
    g_assert_cmpint(copy->ifindex, ==, dev->ifindex);
    g_assert_cmpstr(copy->hw_address, ==, dev->hw_address);
    g_assert_cmpint(copy->state, ==, dev->state);
    g_assert_cmpstr(copy->ssid, ==, dev->ssid);
    g_assert_cmpuint(copy->signal_strength, ==, dev->signal_strength);
    g_assert_cmpuint(copy->frequency, ==, dev->frequency);
    g_assert_cmpstr(copy->ipv4_address, ==, dev->ipv4_address);
    g_assert_cmpstr(copy->ipv4_gateway, ==, dev->ipv4_gateway);

    /* Modify original to ensure deep copy isolation */
    g_free(dev->ssid);
    dev->ssid = g_strdup("Changed_SSID");
    g_assert_cmpstr(copy->ssid, ==, "PTIT_WIFI");

    /* Format string check */
    gchar *desc = multiwifi_device_info_to_string(copy);
    g_assert_nonnull(desc);
    g_assert_nonnull(strstr(desc, "wlp0s20f0u1"));
    g_assert_nonnull(strstr(desc, "PTIT_WIFI"));
    g_assert_nonnull(strstr(desc, "10.10.20.15/24"));
    g_free(desc);

    multiwifi_device_info_free(copy);
    multiwifi_device_info_free(dev);
}

static void test_device_state_mapping(void)
{
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_ACTIVATED),
        ==,
        MULTIWIFI_DEVICE_STATE_CONNECTED
    );
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_DISCONNECTED),
        ==,
        MULTIWIFI_DEVICE_STATE_DISCONNECTED
    );
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_UNAVAILABLE),
        ==,
        MULTIWIFI_DEVICE_STATE_UNAVAILABLE
    );
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_PREPARE),
        ==,
        MULTIWIFI_DEVICE_STATE_PREPARING
    );
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_NEED_AUTH),
        ==,
        MULTIWIFI_DEVICE_STATE_NEED_AUTH
    );
    g_assert_cmpint(
        multiwifi_device_state_from_nm(NM_DEVICE_STATE_FAILED),
        ==,
        MULTIWIFI_DEVICE_STATE_FAILED
    );

    g_assert_cmpstr(multiwifi_device_state_to_string(MULTIWIFI_DEVICE_STATE_CONNECTED), ==, "Connected");
    g_assert_cmpstr(multiwifi_device_state_to_string(MULTIWIFI_DEVICE_STATE_DISCONNECTED), ==, "Disconnected");
    g_assert_cmpstr(multiwifi_device_state_to_string(MULTIWIFI_DEVICE_STATE_UNAVAILABLE), ==, "Unavailable");
}

static void test_inventory_object_lifecycle(void)
{
    MultiWifiInventory *inv = multiwifi_inventory_new();
    g_assert_nonnull(inv);
    g_assert_true(MULTIWIFI_IS_INVENTORY(inv));

    /* Initially not running before start */
    g_assert_false(multiwifi_inventory_is_running(inv));

    GPtrArray *devs = multiwifi_inventory_dup_devices(inv);
    g_assert_nonnull(devs);
    g_assert_cmpuint(devs->len, ==, 0);
    g_ptr_array_unref(devs);

    g_object_unref(inv);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/wifi/device_info_lifecycle", test_device_info_lifecycle);
    g_test_add_func("/wifi/device_state_mapping", test_device_state_mapping);
    g_test_add_func("/wifi/inventory_lifecycle", test_inventory_object_lifecycle);

    return g_test_run();
}
