#include "multiwifi/core/firewall.h"
#include <gio/gio.h>
#include <nftables/libnftables.h>
#include <string.h>

struct _MultiWifiFirewall {
    GObject parent_instance;
    gboolean is_active;
    GMutex mutex;
};

G_DEFINE_TYPE(MultiWifiFirewall, multiwifi_firewall, G_TYPE_OBJECT)

static void multiwifi_firewall_dispose(GObject *object)
{
    MultiWifiFirewall *self = MULTIWIFI_FIREWALL(object);
    g_mutex_clear(&self->mutex);
    G_OBJECT_CLASS(multiwifi_firewall_parent_class)->dispose(object);
}

static void multiwifi_firewall_class_init(MultiWifiFirewallClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_firewall_dispose;
}

static void multiwifi_firewall_init(MultiWifiFirewall *self)
{
    self->is_active = FALSE;
    g_mutex_init(&self->mutex);
}

MultiWifiFirewall *multiwifi_firewall_new(void)
{
    return g_object_new(MULTIWIFI_TYPE_FIREWALL, NULL);
}

gboolean multiwifi_firewall_apply(MultiWifiFirewall *self,
                                  GArray *marks,
                                  GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Firewall manager is NULL");
        return FALSE;
    }
    if (marks == NULL || marks->len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No marks provided for load balancing");
        return FALSE;
    }

    GString *rules = g_string_new("");
    g_string_append(rules, "add table inet " MULTIWIFI_NFT_TABLE_NAME "\n");
    g_string_append(rules, "flush table inet " MULTIWIFI_NFT_TABLE_NAME "\n");
    g_string_append(rules, "table inet " MULTIWIFI_NFT_TABLE_NAME " {\n");
    g_string_append(rules, "    chain output {\n");
    g_string_append(rules, "        type filter hook output priority -150; policy accept;\n");
    g_string_append(rules, "        oif \"lo\" accept\n");
    g_string_append(rules, "        ip daddr { 127.0.0.0/8, 224.0.0.0/4, 255.255.255.255 } accept\n");
    g_string_append(rules, "        ip6 daddr { ::1, fe80::/10, ff00::/8 } accept\n");
    g_string_append(rules, "        ip daddr { 192.168.0.0/16, 10.0.0.0/8, 172.16.0.0/12 } udp dport 53 accept\n");
    g_string_append(rules, "        udp dport { 67, 68, 546, 547 } accept\n");
    g_string_append(rules, "        meta mark & 0xff00 != 0 accept\n");
    g_string_append(rules, "        ip6 version 6 drop\n");

    if (marks->len == 1) {
        uint32_t single_mark = g_array_index(marks, uint32_t, 0);
        g_string_append_printf(rules, "        meta mark set 0x%x\n", single_mark);
    } else {
        const guint NUM_BUCKETS = 64;
        /* Find distinct marks */
        GArray *distinct_marks = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        for (guint i = 0; i < marks->len; i++) {
            uint32_t m = g_array_index(marks, uint32_t, i);
            gboolean found = FALSE;
            for (guint j = 0; j < distinct_marks->len; j++) {
                if (g_array_index(distinct_marks, uint32_t, j) == m) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                g_array_append_val(distinct_marks, m);
            }
        }

        /* Build rules for each distinct mark */
        for (guint d = 0; d < distinct_marks->len; d++) {
            uint32_t target_mark = g_array_index(distinct_marks, uint32_t, d);
            GString *bucket_set = g_string_new("");
            guint count = 0;

            for (guint b = 0; b < NUM_BUCKETS; b++) {
                guint slot = (b * marks->len) / NUM_BUCKETS;
                uint32_t m = g_array_index(marks, uint32_t, slot);
                if (m == target_mark) {
                    if (count > 0) {
                        g_string_append(bucket_set, ", ");
                    }
                    g_string_append_printf(bucket_set, "%u", b);
                    count++;
                }
            }

            if (count > 0) {
                g_string_append_printf(rules,
                    "        tcp sport & 0x3f == { %s } meta mark set 0x%x\n",
                    bucket_set->str, target_mark);
                g_string_append_printf(rules,
                    "        udp sport & 0x3f == { %s } meta mark set 0x%x\n",
                    bucket_set->str, target_mark);
            }
            g_string_free(bucket_set, TRUE);
        }

        g_array_unref(distinct_marks);

        /* Fallback for non-TCP/UDP traffic */
        uint32_t default_mark = g_array_index(marks, uint32_t, 0);
        g_string_append_printf(rules, "        meta mark 0 meta mark set 0x%x\n", default_mark);
    }

    g_string_append(rules, "    }\n}\n");

    struct nft_ctx *nft = nft_ctx_new(NFT_CTX_DEFAULT);
    if (nft == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create libnftables context");
        g_string_free(rules, TRUE);
        return FALSE;
    }

    nft_ctx_buffer_output(nft);
    nft_ctx_buffer_error(nft);

    int rc = nft_run_cmd_from_buffer(nft, rules->str);
    if (rc != 0) {
        const char *err_msg = nft_ctx_get_error_buffer(nft);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "nftables apply failed (rc=%d): %s",
                    rc, (err_msg && *err_msg) ? err_msg : "Unknown error");
        nft_ctx_free(nft);
        g_string_free(rules, TRUE);
        return FALSE;
    }

    nft_ctx_free(nft);
    g_string_free(rules, TRUE);

    /* Phase 2: Postrouting SNAT/Masquerade for multi-WAN and foreign ISP compatibility */
    const char *nat_rules =
        "table ip " MULTIWIFI_NFT_TABLE_NAME " {\n"
        "    chain postrouting {\n"
        "        type nat hook postrouting priority 100; policy accept;\n"
        "        oif != \"lo\" meta mark != 0 masquerade\n"
        "    }\n"
        "}\n";
    struct nft_ctx *nft_nat = nft_ctx_new(NFT_CTX_DEFAULT);
    if (nft_nat != NULL) {
        nft_ctx_buffer_output(nft_nat);
        nft_ctx_buffer_error(nft_nat);
        int nat_rc = nft_run_cmd_from_buffer(nft_nat, nat_rules);
        if (nat_rc == 0) {
            g_info("Multi-WAN NAT masquerade chain applied successfully");
        } else {
            const char *nat_err = nft_ctx_get_error_buffer(nft_nat);
            g_debug("NAT masquerade chain bypassed (kernel nat module unavailable in namespace: %s)",
                    nat_err ? nat_err : "none");
        }
        nft_ctx_free(nft_nat);
    }

    g_mutex_lock(&self->mutex);
    self->is_active = TRUE;
    g_mutex_unlock(&self->mutex);

    return TRUE;
}

gboolean multiwifi_firewall_teardown(MultiWifiFirewall *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Firewall manager is NULL");
        return FALSE;
    }

    struct nft_ctx *nft = nft_ctx_new(NFT_CTX_DEFAULT);
    if (nft == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create libnftables context");
        return FALSE;
    }

    nft_ctx_buffer_output(nft);
    nft_ctx_buffer_error(nft);

    /* Unconditionally delete both the inet filter table and the ip nat masquerade table */
    const char *cmd_inet = "delete table inet " MULTIWIFI_NFT_TABLE_NAME "\n";
    nft_run_cmd_from_buffer(nft, cmd_inet);

    const char *cmd_ip = "delete table ip " MULTIWIFI_NFT_TABLE_NAME "\n";
    nft_run_cmd_from_buffer(nft, cmd_ip);

    nft_ctx_free(nft);
    g_mutex_lock(&self->mutex);
    self->is_active = FALSE;
    g_mutex_unlock(&self->mutex);

    return TRUE;
}

gboolean multiwifi_firewall_verify(MultiWifiFirewall *self,
                                   gboolean *exists,
                                   GError **error)
{
    if (self == NULL || exists == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments for verify");
        return FALSE;
    }

    *exists = FALSE;

    struct nft_ctx *nft = nft_ctx_new(NFT_CTX_DEFAULT);
    if (nft == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create libnftables context");
        return FALSE;
    }

    nft_ctx_buffer_output(nft);
    nft_ctx_buffer_error(nft);

    const char *cmd1 = "list table inet " MULTIWIFI_NFT_TABLE_NAME "\n";
    int rc1 = nft_run_cmd_from_buffer(nft, cmd1);

    const char *cmd2 = "list table ip " MULTIWIFI_NFT_TABLE_NAME "\n";
    int rc2 = nft_run_cmd_from_buffer(nft, cmd2);

    if (rc1 == 0 || rc2 == 0) {
        *exists = TRUE;
    } else {
        *exists = FALSE;
    }

    nft_ctx_free(nft);
    return TRUE;
}

gboolean multiwifi_firewall_is_active(MultiWifiFirewall *self)
{
    if (self == NULL) {
        return FALSE;
    }
    gboolean active;
    g_mutex_lock(&self->mutex);
    active = self->is_active;
    g_mutex_unlock(&self->mutex);
    return active;
}
