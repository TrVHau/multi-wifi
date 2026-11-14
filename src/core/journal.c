#include "multiwifi/core/journal.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

struct _MultiWifiRouteJournal {
    gchar *op_id;
    GPtrArray *entries; /* MultiWifiJournalEntry* */
};

MultiWifiJournalEntry *multiwifi_journal_entry_new(MultiWifiJournalOpType op_type,
                                                   uint32_t table_id,
                                                   uint32_t rule_pref,
                                                   uint32_t fwmark,
                                                   uint32_t fwmask,
                                                   const gchar *src_ip,
                                                   const gchar *gateway,
                                                   const gchar *ifname,
                                                   uint32_t metric)
{
    MultiWifiJournalEntry *entry = g_new0(MultiWifiJournalEntry, 1);
    entry->op_type = op_type;
    entry->table_id = table_id;
    entry->rule_pref = rule_pref;
    entry->fwmark = fwmark;
    entry->fwmask = fwmask;
    entry->src_ip = g_strdup(src_ip);
    entry->gateway = g_strdup(gateway);
    entry->ifname = g_strdup(ifname);
    entry->metric = metric;
    entry->timestamp_us = g_get_monotonic_time();
    return entry;
}

void multiwifi_journal_entry_free(MultiWifiJournalEntry *entry)
{
    if (entry == NULL) {
        return;
    }

    g_free(entry->src_ip);
    g_free(entry->gateway);
    g_free(entry->ifname);
    g_free(entry);
}

MultiWifiRouteJournal *multiwifi_route_journal_new(const gchar *op_id)
{
    MultiWifiRouteJournal *journal = g_new0(MultiWifiRouteJournal, 1);
    journal->op_id = g_strdup(op_id ? op_id : "unknown");
    journal->entries = g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_journal_entry_free);
    return journal;
}

void multiwifi_route_journal_free(MultiWifiRouteJournal *journal)
{
    if (journal == NULL) {
        return;
    }

    g_free(journal->op_id);
    if (journal->entries != NULL) {
        g_ptr_array_unref(journal->entries);
    }
    g_free(journal);
}

const gchar *multiwifi_route_journal_get_op_id(const MultiWifiRouteJournal *journal)
{
    return journal ? journal->op_id : "";
}

void multiwifi_route_journal_append(MultiWifiRouteJournal *journal,
                                    MultiWifiJournalEntry *entry)
{
    if (journal == NULL || entry == NULL) {
        return;
    }
    g_ptr_array_add(journal->entries, entry);
}

GPtrArray *multiwifi_route_journal_get_entries(const MultiWifiRouteJournal *journal)
{
    return journal ? journal->entries : NULL;
}

gboolean multiwifi_route_journal_save_to_file(const MultiWifiRouteJournal *journal,
                                              const gchar *file_path,
                                              GError **error)
{
    if (journal == NULL || file_path == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to save journal");
        return FALSE;
    }

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "journal", "op_id", journal->op_id);
    g_key_file_set_integer(kf, "journal", "count", (gint)journal->entries->len);

    for (guint i = 0; i < journal->entries->len; i++) {
        MultiWifiJournalEntry *e = g_ptr_array_index(journal->entries, i);
        gchar *group = g_strdup_printf("entry_%u", i);

        g_key_file_set_integer(kf, group, "op_type", (gint)e->op_type);
        g_key_file_set_int64(kf, group, "table_id", (gint64)e->table_id);
        g_key_file_set_int64(kf, group, "rule_pref", (gint64)e->rule_pref);
        g_key_file_set_int64(kf, group, "fwmark", (gint64)e->fwmark);
        g_key_file_set_int64(kf, group, "fwmask", (gint64)e->fwmask);
        g_key_file_set_string(kf, group, "src_ip", e->src_ip ? e->src_ip : "");
        g_key_file_set_string(kf, group, "gateway", e->gateway ? e->gateway : "");
        g_key_file_set_string(kf, group, "ifname", e->ifname ? e->ifname : "");
        g_key_file_set_int64(kf, group, "metric", (gint64)e->metric);
        g_key_file_set_int64(kf, group, "timestamp_us", e->timestamp_us);

        g_free(group);
    }

    /* Ensure parent directory exists */
    gchar *dirname = g_path_get_dirname(file_path);
    if (g_mkdir_with_parents(dirname, 0700) != 0) {
        /* If directory creation failed and not EEXIST */
        if (errno != EEXIST) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to create journal directory '%s': %s", dirname, g_strerror(errno));
            g_free(dirname);
            g_key_file_free(kf);
            return FALSE;
        }
    }
    g_free(dirname);

    gboolean res = g_key_file_save_to_file(kf, file_path, error);
    g_key_file_free(kf);
    return res;
}

MultiWifiRouteJournal *multiwifi_route_journal_load_from_file(const gchar *file_path,
                                                              GError **error)
{
    if (file_path == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "File path is NULL");
        return NULL;
    }

    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, file_path, G_KEY_FILE_NONE, error)) {
        g_key_file_free(kf);
        return NULL;
    }

    gchar *op_id = g_key_file_get_string(kf, "journal", "op_id", NULL);
    gint count = g_key_file_get_integer(kf, "journal", "count", NULL);

    MultiWifiRouteJournal *journal = multiwifi_route_journal_new(op_id ? op_id : "unknown");
    g_free(op_id);

    for (gint i = 0; i < count; i++) {
        gchar *group = g_strdup_printf("entry_%d", i);
        if (g_key_file_has_group(kf, group)) {
            MultiWifiJournalOpType op_type = (MultiWifiJournalOpType)g_key_file_get_integer(kf, group, "op_type", NULL);
            uint32_t table_id = (uint32_t)g_key_file_get_int64(kf, group, "table_id", NULL);
            uint32_t rule_pref = (uint32_t)g_key_file_get_int64(kf, group, "rule_pref", NULL);
            uint32_t fwmark = (uint32_t)g_key_file_get_int64(kf, group, "fwmark", NULL);
            uint32_t fwmask = (uint32_t)g_key_file_get_int64(kf, group, "fwmask", NULL);
            gchar *src_ip = g_key_file_get_string(kf, group, "src_ip", NULL);
            gchar *gateway = g_key_file_get_string(kf, group, "gateway", NULL);
            gchar *ifname = g_key_file_get_string(kf, group, "ifname", NULL);
            uint32_t metric = (uint32_t)g_key_file_get_int64(kf, group, "metric", NULL);

            MultiWifiJournalEntry *entry = multiwifi_journal_entry_new(
                op_type, table_id, rule_pref, fwmark, fwmask,
                src_ip, gateway, ifname, metric
            );
            entry->timestamp_us = g_key_file_get_int64(kf, group, "timestamp_us", NULL);

            multiwifi_route_journal_append(journal, entry);

            g_free(src_ip);
            g_free(gateway);
            g_free(ifname);
        }
        g_free(group);
    }

    g_key_file_free(kf);
    return journal;
}

gboolean multiwifi_route_journal_remove_file(const gchar *file_path)
{
    if (file_path == NULL) {
        return FALSE;
    }
    if (g_file_test(file_path, G_FILE_TEST_EXISTS)) {
        return (g_unlink(file_path) == 0);
    }
    return TRUE;
}
