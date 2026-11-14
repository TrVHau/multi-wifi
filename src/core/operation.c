#include "multiwifi/core/operation.h"
#include <gio/gio.h>

enum {
    SIGNAL_OP_PROGRESS,
    SIGNAL_OP_FINISHED,
    LAST_SIGNAL
};

static guint s_signals[LAST_SIGNAL] = { 0 };

struct _MultiWifiOpManager {
    GObject parent_instance;
    GPtrArray *operations; /* MultiWifiOperation* */
    GMutex mutex;
};

G_DEFINE_TYPE(MultiWifiOpManager, multiwifi_op_manager, G_TYPE_OBJECT)

MultiWifiOperation *multiwifi_operation_new(const gchar *action)
{
    MultiWifiOperation *op = g_new0(MultiWifiOperation, 1);
    op->operation_id = g_uuid_string_random();
    op->action = g_strdup(action ? action : "unknown");
    op->state = MULTIWIFI_OP_STATE_QUEUED;
    op->percent = 0;
    op->message = g_strdup("Operation queued");
    op->success = FALSE;
    op->result = NULL;
    op->created_at_us = g_get_monotonic_time();
    op->finished_at_us = 0;
    return op;
}

MultiWifiOperation *multiwifi_operation_copy(const MultiWifiOperation *src)
{
    if (src == NULL) {
        return NULL;
    }

    MultiWifiOperation *dst = g_new0(MultiWifiOperation, 1);
    dst->operation_id = g_strdup(src->operation_id);
    dst->action = g_strdup(src->action);
    dst->state = src->state;
    dst->percent = src->percent;
    dst->message = g_strdup(src->message);
    dst->success = src->success;
    dst->result = src->result ? g_variant_ref(src->result) : NULL;
    dst->created_at_us = src->created_at_us;
    dst->finished_at_us = src->finished_at_us;
    return dst;
}

void multiwifi_operation_free(MultiWifiOperation *op)
{
    if (op == NULL) {
        return;
    }

    g_free(op->operation_id);
    g_free(op->action);
    g_free(op->message);
    if (op->result != NULL) {
        g_variant_unref(op->result);
    }
    g_free(op);
}

const gchar *multiwifi_op_state_to_string(MultiWifiOpState state)
{
    switch (state) {
    case MULTIWIFI_OP_STATE_QUEUED:
        return "QUEUED";
    case MULTIWIFI_OP_STATE_VALIDATING:
        return "VALIDATING";
    case MULTIWIFI_OP_STATE_APPLYING:
        return "APPLYING";
    case MULTIWIFI_OP_STATE_VERIFYING:
        return "VERIFYING";
    case MULTIWIFI_OP_STATE_SUCCEEDED:
        return "SUCCEEDED";
    case MULTIWIFI_OP_STATE_ROLLING_BACK:
        return "ROLLING_BACK";
    case MULTIWIFI_OP_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

static void multiwifi_op_manager_dispose(GObject *object)
{
    MultiWifiOpManager *self = MULTIWIFI_OP_MANAGER(object);

    g_mutex_lock(&self->mutex);
    if (self->operations != NULL) {
        g_ptr_array_unref(self->operations);
        self->operations = NULL;
    }
    g_mutex_unlock(&self->mutex);

    G_OBJECT_CLASS(multiwifi_op_manager_parent_class)->dispose(object);
}

static void multiwifi_op_manager_finalize(GObject *object)
{
    MultiWifiOpManager *self = MULTIWIFI_OP_MANAGER(object);
    g_mutex_clear(&self->mutex);
    G_OBJECT_CLASS(multiwifi_op_manager_parent_class)->finalize(object);
}

static void multiwifi_op_manager_class_init(MultiWifiOpManagerClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = multiwifi_op_manager_dispose;
    oclass->finalize = multiwifi_op_manager_finalize;

    s_signals[SIGNAL_OP_PROGRESS] = g_signal_new(
        "operation-progress",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL,
        NULL,
        NULL,
        G_TYPE_NONE,
        3,
        G_TYPE_STRING,
        G_TYPE_UINT,
        G_TYPE_STRING
    );

    s_signals[SIGNAL_OP_FINISHED] = g_signal_new(
        "operation-finished",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL,
        NULL,
        NULL,
        G_TYPE_NONE,
        3,
        G_TYPE_STRING,
        G_TYPE_BOOLEAN,
        G_TYPE_VARIANT
    );
}

static void multiwifi_op_manager_init(MultiWifiOpManager *self)
{
    g_mutex_init(&self->mutex);
    self->operations = g_ptr_array_new_with_free_func((GDestroyNotify)multiwifi_operation_free);
}

MultiWifiOpManager *multiwifi_op_manager_new(void)
{
    return g_object_new(MULTIWIFI_TYPE_OP_MANAGER, NULL);
}

gchar *multiwifi_op_manager_create_op(MultiWifiOpManager *self, const gchar *action)
{
    if (self == NULL) {
        return NULL;
    }

    MultiWifiOperation *op = multiwifi_operation_new(action);
    gchar *op_id = g_strdup(op->operation_id);

    g_mutex_lock(&self->mutex);
    g_ptr_array_add(self->operations, op);
    g_mutex_unlock(&self->mutex);

    return op_id;
}

static MultiWifiOperation *find_op_locked(MultiWifiOpManager *self, const gchar *op_id)
{
    if (self == NULL || op_id == NULL) {
        return NULL;
    }

    for (guint i = 0; i < self->operations->len; i++) {
        MultiWifiOperation *op = g_ptr_array_index(self->operations, i);
        if (g_strcmp0(op->operation_id, op_id) == 0) {
            return op;
        }
    }
    return NULL;
}

gboolean multiwifi_op_manager_update(MultiWifiOpManager *self,
                                     const gchar *op_id,
                                     MultiWifiOpState state,
                                     guint percent,
                                     const gchar *message)
{
    if (self == NULL || op_id == NULL) {
        return FALSE;
    }

    g_mutex_lock(&self->mutex);
    MultiWifiOperation *op = find_op_locked(self, op_id);
    if (op == NULL) {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    op->state = state;
    op->percent = (percent > 100) ? 100 : percent;
    g_free(op->message);
    op->message = g_strdup(message ? message : "");
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_OP_PROGRESS], 0, op_id, percent, message ? message : "");
    return TRUE;
}

gboolean multiwifi_op_manager_finish(MultiWifiOpManager *self,
                                     const gchar *op_id,
                                     gboolean success,
                                     GVariant *result,
                                     const gchar *message)
{
    if (self == NULL || op_id == NULL) {
        return FALSE;
    }

    GVariant *result_copy = NULL;
    if (result != NULL) {
        result_copy = g_variant_ref_sink(result);
    } else {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        result_copy = g_variant_ref_sink(g_variant_builder_end(&b));
    }

    g_mutex_lock(&self->mutex);
    MultiWifiOperation *op = find_op_locked(self, op_id);
    if (op == NULL) {
        g_mutex_unlock(&self->mutex);
        g_variant_unref(result_copy);
        return FALSE;
    }

    op->state = success ? MULTIWIFI_OP_STATE_SUCCEEDED : MULTIWIFI_OP_STATE_FAILED;
    op->percent = 100;
    op->success = success;
    op->finished_at_us = g_get_monotonic_time();
    g_free(op->message);
    op->message = g_strdup(message ? message : (success ? "Succeeded" : "Failed"));

    if (op->result != NULL) {
        g_variant_unref(op->result);
    }
    op->result = g_variant_ref(result_copy);
    g_mutex_unlock(&self->mutex);

    g_signal_emit(self, s_signals[SIGNAL_OP_FINISHED], 0, op_id, success, result_copy);
    g_variant_unref(result_copy);
    return TRUE;
}

MultiWifiOperation *multiwifi_op_manager_find(MultiWifiOpManager *self,
                                              const gchar *op_id)
{
    if (self == NULL || op_id == NULL) {
        return NULL;
    }

    MultiWifiOperation *copy = NULL;
    g_mutex_lock(&self->mutex);
    MultiWifiOperation *op = find_op_locked(self, op_id);
    if (op != NULL) {
        copy = multiwifi_operation_copy(op);
    }
    g_mutex_unlock(&self->mutex);

    return copy;
}

GVariant *multiwifi_op_manager_to_variant(MultiWifiOpManager *self,
                                          const gchar *op_id)
{
    MultiWifiOperation *op = multiwifi_op_manager_find(self, op_id);
    if (op == NULL) {
        return NULL;
    }

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "operation_id", g_variant_new_string(op->operation_id));
    g_variant_builder_add(&b, "{sv}", "action", g_variant_new_string(op->action));
    g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string(multiwifi_op_state_to_string(op->state)));
    g_variant_builder_add(&b, "{sv}", "percent", g_variant_new_uint32(op->percent));
    g_variant_builder_add(&b, "{sv}", "message", g_variant_new_string(op->message ? op->message : ""));
    g_variant_builder_add(&b, "{sv}", "success", g_variant_new_boolean(op->success));

    if (op->result != NULL) {
        g_variant_builder_add(&b, "{sv}", "result", op->result);
    }

    multiwifi_operation_free(op);
    return g_variant_builder_end(&b);
}
