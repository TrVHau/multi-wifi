#ifndef MULTIWIFI_JOURNAL_H
#define MULTIWIFI_JOURNAL_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_JOURNAL_OP_ADD_RULE = 1,
    MULTIWIFI_JOURNAL_OP_ADD_ROUTE = 2,
    MULTIWIFI_JOURNAL_OP_ALLOC_TABLE = 3
} MultiWifiJournalOpType;

typedef struct {
    MultiWifiJournalOpType op_type;
    uint32_t table_id;
    uint32_t rule_pref;
    uint32_t fwmark;
    uint32_t fwmask;
    gchar *src_ip;   /* (owned)(nullable) */
    gchar *gateway;  /* (owned)(nullable) */
    gchar *ifname;   /* (owned)(nullable) */
    uint32_t metric;
    gint64 timestamp_us;
} MultiWifiJournalEntry;

typedef struct _MultiWifiRouteJournal MultiWifiRouteJournal;

/**
 * multiwifi_journal_entry_new:
 *
 * Allocates and initializes a new journal entry.
 *
 * Returns: (transfer full): A new MultiWifiJournalEntry instance.
 */
MultiWifiJournalEntry *multiwifi_journal_entry_new(MultiWifiJournalOpType op_type,
                                                   uint32_t table_id,
                                                   uint32_t rule_pref,
                                                   uint32_t fwmark,
                                                   uint32_t fwmask,
                                                   const gchar *src_ip,
                                                   const gchar *gateway,
                                                   const gchar *ifname,
                                                   uint32_t metric);

/**
 * multiwifi_journal_entry_free:
 * @entry: (transfer full)(nullable): Entry to free
 */
void multiwifi_journal_entry_free(MultiWifiJournalEntry *entry);

/**
 * multiwifi_route_journal_new:
 * @op_id: Operation UUID
 *
 * Creates a new transaction journal for tracking network changes.
 *
 * Returns: (transfer full): A new MultiWifiRouteJournal object.
 */
MultiWifiRouteJournal *multiwifi_route_journal_new(const gchar *op_id);

/**
 * multiwifi_route_journal_free:
 * @journal: (transfer full)(nullable): Journal to free
 */
void multiwifi_route_journal_free(MultiWifiRouteJournal *journal);

/**
 * multiwifi_route_journal_get_op_id:
 * @journal: Route journal
 *
 * Returns: (transfer none): Operation UUID string.
 */
const gchar *multiwifi_route_journal_get_op_id(const MultiWifiRouteJournal *journal);

/**
 * multiwifi_route_journal_append:
 * @journal: Route journal
 * @entry: (transfer full): Entry to append
 */
void multiwifi_route_journal_append(MultiWifiRouteJournal *journal,
                                    MultiWifiJournalEntry *entry);

/**
 * multiwifi_route_journal_get_entries:
 * @journal: Route journal
 *
 * Returns: (transfer none): Internal GPtrArray of MultiWifiJournalEntry*.
 */
GPtrArray *multiwifi_route_journal_get_entries(const MultiWifiRouteJournal *journal);

/**
 * multiwifi_route_journal_save_to_file:
 * @journal: Route journal
 * @file_path: Absolute destination path
 * @error: (out)(optional): Error location
 *
 * Persists journal entries to disk in atomic JSON format.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean multiwifi_route_journal_save_to_file(const MultiWifiRouteJournal *journal,
                                              const gchar *file_path,
                                              GError **error);

/**
 * multiwifi_route_journal_load_from_file:
 * @file_path: Path to existing journal file
 * @error: (out)(optional): Error location
 *
 * Parses and loads a previously persisted journal file.
 *
 * Returns: (transfer full)(nullable): Loaded MultiWifiRouteJournal or %NULL.
 */
MultiWifiRouteJournal *multiwifi_route_journal_load_from_file(const gchar *file_path,
                                                              GError **error);

/**
 * multiwifi_route_journal_remove_file:
 * @file_path: Path to remove
 *
 * Safely removes a committed or cleaned up journal file.
 *
 * Returns: %TRUE if deleted or did not exist, %FALSE on failure.
 */
gboolean multiwifi_route_journal_remove_file(const gchar *file_path);

G_END_DECLS

#endif /* MULTIWIFI_JOURNAL_H */
