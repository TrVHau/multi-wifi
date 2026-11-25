#include "multiwifi/core/operation.h"
#include <glib.h>
#include <string.h>

static void test_operation_manager_lifecycle(void)
{
    MultiWifiOpManager *mgr = multiwifi_op_manager_new();
    g_assert_nonnull(mgr);

    gchar *op_id = multiwifi_op_manager_create_op(mgr, "TestOperation");
    g_assert_nonnull(op_id);
    g_assert_cmpuint(strlen(op_id), >, 10);

    /* Verify initial state: QUEUED */
    MultiWifiOperation *op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_nonnull(op);
    g_assert_cmpstr(op->action, ==, "TestOperation");
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_QUEUED);
    g_assert_cmpuint(op->percent, ==, 0);
    g_assert_false(op->success);
    multiwifi_operation_free(op);

    /* Update progress: VALIDATING */
    gboolean updated = multiwifi_op_manager_update(mgr, op_id, MULTIWIFI_OP_STATE_VALIDATING, 25, "Validating parameters");
    g_assert_true(updated);

    op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_VALIDATING);
    g_assert_cmpuint(op->percent, ==, 25);
    g_assert_cmpstr(op->message, ==, "Validating parameters");
    multiwifi_operation_free(op);

    /* Finish operation: SUCCEEDED */
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "detail", g_variant_new_string("completed cleanly"));
    GVariant *res = g_variant_builder_end(&b);

    gboolean finished = multiwifi_op_manager_finish(mgr, op_id, TRUE, res, "Completed");
    g_assert_true(finished);

    op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_SUCCEEDED);
    g_assert_cmpuint(op->percent, ==, 100);
    g_assert_true(op->success);
    g_assert_cmpstr(op->message, ==, "Completed");
    multiwifi_operation_free(op);

    /* Test variant serialization */
    GVariant *var = multiwifi_op_manager_to_variant(mgr, op_id);
    g_assert_nonnull(var);
    g_variant_unref(var);

    g_free(op_id);
    g_object_unref(mgr);
}

static void test_operation_rollback_lifecycle(void)
{
    MultiWifiOpManager *mgr = multiwifi_op_manager_new();
    g_assert_nonnull(mgr);

    gchar *op_id = multiwifi_op_manager_create_op(mgr, "FailedMutation");
    g_assert_nonnull(op_id);

    /* Stage 1: VALIDATING */
    g_assert_true(multiwifi_op_manager_update(mgr, op_id, MULTIWIFI_OP_STATE_VALIDATING, 10, "Validating routes"));
    MultiWifiOperation *op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_VALIDATING);
    multiwifi_operation_free(op);

    /* Stage 2: APPLYING */
    g_assert_true(multiwifi_op_manager_update(mgr, op_id, MULTIWIFI_OP_STATE_APPLYING, 50, "Applying policy rules"));
    op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_APPLYING);
    multiwifi_operation_free(op);

    /* Stage 3: ROLLING_BACK on conflict/failure */
    g_assert_true(multiwifi_op_manager_update(mgr, op_id, MULTIWIFI_OP_STATE_ROLLING_BACK, 75, "Rolling back applied tables"));
    op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_ROLLING_BACK);
    multiwifi_operation_free(op);

    /* Stage 4: FAILED */
    g_assert_true(multiwifi_op_manager_finish(mgr, op_id, FALSE, NULL, "Rollback completed, operation aborted"));
    op = multiwifi_op_manager_find(mgr, op_id);
    g_assert_cmpint(op->state, ==, MULTIWIFI_OP_STATE_FAILED);
    g_assert_false(op->success);
    g_assert_cmpstr(op->message, ==, "Rollback completed, operation aborted");
    multiwifi_operation_free(op);

    g_free(op_id);
    g_object_unref(mgr);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/operation/manager_lifecycle", test_operation_manager_lifecycle);
    g_test_add_func("/operation/rollback_lifecycle", test_operation_rollback_lifecycle);

    return g_test_run();
}
