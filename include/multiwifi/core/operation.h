#ifndef MULTIWIFI_OPERATION_H
#define MULTIWIFI_OPERATION_H

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
    MULTIWIFI_OP_STATE_QUEUED = 0,
    MULTIWIFI_OP_STATE_VALIDATING,
    MULTIWIFI_OP_STATE_APPLYING,
    MULTIWIFI_OP_STATE_VERIFYING,
    MULTIWIFI_OP_STATE_SUCCEEDED,
    MULTIWIFI_OP_STATE_ROLLING_BACK,
    MULTIWIFI_OP_STATE_FAILED
} MultiWifiOpState;

typedef struct {
    gchar *operation_id;    /* UUID string (owned) */
    gchar *action;          /* e.g. "StartLoadBalancing" (owned) */
    MultiWifiOpState state; /* Current lifecycle stage */
    guint percent;          /* 0 - 100% progress */
    gchar *message;         /* Progress/status message (owned) */
    gboolean success;       /* Final outcome */
    GVariant *result;       /* (owned)(nullable): Result dictionary */
    gint64 created_at_us;   /* Monotonic creation timestamp */
    gint64 finished_at_us;  /* Monotonic completion timestamp */
} MultiWifiOperation;

/**
 * multiwifi_operation_new:
 * @action: Action name
 *
 * Allocates a new operation with a unique random UUID.
 *
 * Returns: (transfer full): A new MultiWifiOperation instance.
 */
MultiWifiOperation *multiwifi_operation_new(const gchar *action);

/**
 * multiwifi_operation_copy:
 * @src: Source operation to duplicate
 *
 * Returns: (transfer full): Deep copy of @src.
 */
MultiWifiOperation *multiwifi_operation_copy(const MultiWifiOperation *src);

/**
 * multiwifi_operation_free:
 * @op: (transfer full)(nullable): Struct to free
 */
void multiwifi_operation_free(MultiWifiOperation *op);

/**
 * multiwifi_op_state_to_string:
 * @state: Operation state
 *
 * Returns: (transfer none): Static human-readable state string.
 */
const gchar *multiwifi_op_state_to_string(MultiWifiOpState state);

#define MULTIWIFI_TYPE_OP_MANAGER (multiwifi_op_manager_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiOpManager, multiwifi_op_manager, MULTIWIFI, OP_MANAGER, GObject)

/**
 * multiwifi_op_manager_new:
 *
 * Creates a new operation lifecycle manager.
 *
 * Returns: (transfer full): A new MultiWifiOpManager object.
 */
MultiWifiOpManager *multiwifi_op_manager_new(void);

/**
 * multiwifi_op_manager_create_op:
 * @self: Operation manager
 * @action: Action name, e.g. "StartLoadBalancing"
 *
 * Registers a new queued operation.
 *
 * Returns: (transfer full): Generated unique operation UUID string.
 */
gchar *multiwifi_op_manager_create_op(MultiWifiOpManager *self, const gchar *action);

/**
 * multiwifi_op_manager_update:
 * @self: Operation manager
 * @op_id: Operation UUID
 * @state: New lifecycle state
 * @percent: Progress percentage (0-100)
 * @message: Descriptive progress message
 *
 * Updates operation progress and emits "operation-progress" signal.
 *
 * Returns: %TRUE if operation found and updated, %FALSE otherwise.
 */
gboolean multiwifi_op_manager_update(MultiWifiOpManager *self,
                                     const gchar *op_id,
                                     MultiWifiOpState state,
                                     guint percent,
                                     const gchar *message);

/**
 * multiwifi_op_manager_finish:
 * @self: Operation manager
 * @op_id: Operation UUID
 * @success: Whether operation succeeded
 * @result: (transfer none)(nullable): Result dictionary variant or %NULL
 * @message: Final completion message
 *
 * Completes operation, sets finished timestamp, and emits "operation-finished".
 *
 * Returns: %TRUE if operation found and finalized, %FALSE otherwise.
 */
gboolean multiwifi_op_manager_finish(MultiWifiOpManager *self,
                                     const gchar *op_id,
                                     gboolean success,
                                     GVariant *result,
                                     const gchar *message);

/**
 * multiwifi_op_manager_find:
 * @self: Operation manager
 * @op_id: Operation UUID
 *
 * Returns: (transfer full)(nullable): Deep copy of operation or %NULL.
 */
MultiWifiOperation *multiwifi_op_manager_find(MultiWifiOpManager *self,
                                              const gchar *op_id);

/**
 * multiwifi_op_manager_to_variant:
 * @self: Operation manager
 * @op_id: Operation UUID
 *
 * Returns: (transfer full)(nullable): GVariant dictionary of type a{sv} or %NULL.
 */
GVariant *multiwifi_op_manager_to_variant(MultiWifiOpManager *self,
                                          const gchar *op_id);

G_END_DECLS

#endif /* MULTIWIFI_OPERATION_H */
