#include "multiwifi/core/operation.h"
#include "multiwifi/daemon/service.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

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

typedef struct {
    gboolean done;
    GVariant *reply;
    GError *error;
} AsyncCallCtx;

static void on_async_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    AsyncCallCtx *ctx = (AsyncCallCtx *)user_data;
    ctx->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &ctx->error);
    ctx->done = TRUE;
}

static GVariant *call_dbus_iterative(GDBusConnection *conn,
                                     const gchar *method_name,
                                     GVariant *parameters,
                                     const GVariantType *reply_type,
                                     GError **error)
{
    AsyncCallCtx ctx = { .done = FALSE, .reply = NULL, .error = NULL };

    g_dbus_connection_call(
        conn,
        MULTIWIFI_DBUS_NAME,
        MULTIWIFI_DBUS_PATH,
        MULTIWIFI_DBUS_INTERFACE,
        method_name,
        parameters,
        reply_type,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        on_async_call_done,
        &ctx
    );

    GMainContext *main_ctx = g_main_context_default();
    while (!ctx.done) {
        g_main_context_iteration(main_ctx, TRUE);
    }

    if (ctx.error != NULL) {
        g_propagate_error(error, ctx.error);
        return NULL;
    }

    return ctx.reply;
}

static void test_daemon_dbus_ipc_roundtrip(void)
{
    MultiWifiDaemonService *svc = multiwifi_daemon_service_new(G_BUS_TYPE_SESSION, FALSE);
    g_assert_nonnull(svc);

    GError *error = NULL;
    gboolean started = multiwifi_daemon_service_start(svc, &error);
    g_assert_no_error(error);
    g_assert_true(started);

    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(conn);

    /* Allow the main context to process the bus name ownership */
    GMainContext *ctx = g_main_context_default();
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(10000);
    }

    /* 1. Test GetVersion */
    GVariant *reply = call_dbus_iterative(
        conn,
        "GetVersion",
        NULL,
        G_VARIANT_TYPE("(su)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);

    const gchar *ver_str = NULL;
    guint32 api_ver = 0;
    g_variant_get(reply, "(&su)", &ver_str, &api_ver);
    g_assert_cmpstr(ver_str, ==, MULTIWIFI_VERSION_STRING);
    g_assert_cmpuint(api_ver, ==, MULTIWIFI_API_VERSION);
    g_variant_unref(reply);

    /* 2. Test GetCapabilities */
    reply = call_dbus_iterative(
        conn,
        "GetCapabilities",
        NULL,
        G_VARIANT_TYPE("(a{sv})"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 3. Test GetDevices */
    reply = call_dbus_iterative(
        conn,
        "GetDevices",
        NULL,
        G_VARIANT_TYPE("(aa{sv})"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 4. Test GetPaths */
    reply = call_dbus_iterative(
        conn,
        "GetPaths",
        NULL,
        G_VARIANT_TYPE("(aa{sv})"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 5. Test GetStatus */
    reply = call_dbus_iterative(
        conn,
        "GetStatus",
        NULL,
        G_VARIANT_TYPE("(a{sv})"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 6. Test SetPathWeight validation (failure on weight 0) */
    reply = call_dbus_iterative(
        conn,
        "SetPathWeight",
        g_variant_new("(su)", "wlan0", 0),
        NULL,
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    gchar *remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&error);

    /* 7. Test SetPathWeight validation (failure on empty device id) */
    reply = call_dbus_iterative(
        conn,
        "SetPathWeight",
        g_variant_new("(su)", "", 10),
        NULL,
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&error);

    /* 8. Test SetPathWeight valid call */
    reply = call_dbus_iterative(
        conn,
        "SetPathWeight",
        g_variant_new("(su)", "wlan0", 5),
        NULL,
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 9. Test StartLoadBalancing (if unprivileged on host, kernel EPERM triggers Conflict error) */
    GVariantBuilder opt_b;
    g_variant_builder_init(&opt_b, G_VARIANT_TYPE("a{sv}"));
    reply = call_dbus_iterative(
        conn,
        "StartLoadBalancing",
        g_variant_new("(@a{sv})", g_variant_builder_end(&opt_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    if (reply != NULL) {
        g_variant_unref(reply);
    } else {
        g_assert_nonnull(error);
        g_assert_true(g_dbus_error_is_remote_error(error));
        remote_err = g_dbus_error_get_remote_error(error);
        g_assert_true(g_strcmp0(remote_err, MULTIWIFI_ERROR_CONFLICT) == 0 ||
                      g_strcmp0(remote_err, MULTIWIFI_ERROR_NETWORK_FAILURE) == 0);
        g_free(remote_err);
        g_clear_error(&error);
    }

    /* 10. Test StopLoadBalancing */
    reply = call_dbus_iterative(
        conn,
        "StopLoadBalancing",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 11. Test StartBonding */
    GVariantBuilder bond_b;
    g_variant_builder_init(&bond_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&bond_b, "{sv}", "server_host", g_variant_new_string("127.0.0.1"));
    g_variant_builder_add(&bond_b, "{sv}", "server_port", g_variant_new_uint32(59999));
    g_variant_builder_add(&bond_b, "{sv}", "token", g_variant_new_string("test-token"));
    reply = call_dbus_iterative(
        conn,
        "StartBonding",
        g_variant_new("(@a{sv})", g_variant_builder_end(&bond_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    if (reply != NULL) {
        g_variant_unref(reply);
    } else {
        g_assert_nonnull(error);
        g_assert_true(g_dbus_error_is_remote_error(error));
        remote_err = g_dbus_error_get_remote_error(error);
        g_assert_true(g_strcmp0(remote_err, MULTIWIFI_ERROR_NETWORK_FAILURE) == 0 ||
                      g_strcmp0(remote_err, MULTIWIFI_ERROR_INVALID_ARGUMENT) == 0);
        g_free(remote_err);
        g_clear_error(&error);
    }

    /* 12. Test StopBonding */
    reply = call_dbus_iterative(
        conn,
        "StopBonding",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* 13. Test RunDiagnostics */
    reply = call_dbus_iterative(
        conn,
        "RunDiagnostics",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);

    const gchar *diag_op_id = NULL;
    g_variant_get(reply, "(&s)", &diag_op_id);
    g_assert_nonnull(diag_op_id);
    gchar *saved_diag_op = g_strdup(diag_op_id);
    g_variant_unref(reply);

    /* 14. Test GetDiagnostics (valid) */
    reply = call_dbus_iterative(
        conn,
        "GetDiagnostics",
        g_variant_new("(s)", saved_diag_op),
        G_VARIANT_TYPE("(a{sv})"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);
    g_free(saved_diag_op);

    /* 15. Test GetDiagnostics (invalid / non-existent) */
    reply = call_dbus_iterative(
        conn,
        "GetDiagnostics",
        g_variant_new("(s)", "00000000-0000-0000-0000-000000000000"),
        G_VARIANT_TYPE("(a{sv})"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&error);

    /* 16. Test GetDNS initial */
    reply = call_dbus_iterative(
        conn,
        "GetDNS",
        NULL,
        G_VARIANT_TYPE("(asb)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    GVariantIter *dns_iter = NULL;
    gboolean is_custom = TRUE;
    g_variant_get(reply, "(asb)", &dns_iter, &is_custom);
    g_variant_iter_free(dns_iter);
    g_variant_unref(reply);

    /* Setup temp test dir for DNS files */
    gchar *tmp_dir = g_dir_make_tmp("multiwifi_dns_test_XXXXXX", &error);
    g_assert_no_error(error);
    gchar *fake_resolv = g_build_filename(tmp_dir, "resolv.conf", NULL);
    g_file_set_contents(fake_resolv, "nameserver 192.168.1.1\n", -1, NULL);

    g_setenv("MULTIWIFI_DNS_CONF_DIR", tmp_dir, TRUE);
    g_setenv("MULTIWIFI_RESOLV_CONF", fake_resolv, TRUE);

    /* 17. Test SetDNS invalid IP */
    GVariantBuilder bad_ip_b;
    g_variant_builder_init(&bad_ip_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&bad_ip_b, "s", "invalid.not.an.ip");
    reply = call_dbus_iterative(
        conn,
        "SetDNS",
        g_variant_new("(@as)", g_variant_builder_end(&bad_ip_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&error);

    /* 18. Test SetDNS empty list */
    GVariantBuilder empty_dns_b;
    g_variant_builder_init(&empty_dns_b, G_VARIANT_TYPE("as"));
    reply = call_dbus_iterative(
        conn,
        "SetDNS",
        g_variant_new("(@as)", g_variant_builder_end(&empty_dns_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&error);

    /* 19. Test SetDNS valid list (Cloudflare) */
    GVariantBuilder valid_dns_b;
    g_variant_builder_init(&valid_dns_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&valid_dns_b, "s", "1.1.1.1");
    g_variant_builder_add(&valid_dns_b, "s", "1.0.0.1");
    reply = call_dbus_iterative(
        conn,
        "SetDNS",
        g_variant_new("(@as)", g_variant_builder_end(&valid_dns_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    const gchar *set_res = NULL;
    g_variant_get(reply, "(&s)", &set_res);
    g_assert_cmpstr(set_res, ==, "OK");
    g_variant_unref(reply);

    /* 20. Verify GetDNS reflects custom DNS */
    reply = call_dbus_iterative(
        conn,
        "GetDNS",
        NULL,
        G_VARIANT_TYPE("(asb)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(asb)", &dns_iter, &is_custom);
    g_assert_true(is_custom);
    const gchar *server_val = NULL;
    gboolean found_cf = FALSE;
    while (g_variant_iter_next(dns_iter, "&s", &server_val)) {
        if (g_strcmp0(server_val, "1.1.1.1") == 0) {
            found_cf = TRUE;
        }
    }
    g_assert_true(found_cf);
    g_variant_iter_free(dns_iter);
    g_variant_unref(reply);

    /* 21. Test ResetDNS */
    reply = call_dbus_iterative(
        conn,
        "ResetDNS",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    const gchar *reset_res = NULL;
    g_variant_get(reply, "(&s)", &reset_res);
    g_assert_cmpstr(reset_res, ==, "OK");
    g_variant_unref(reply);

    /* Verify GetDNS reflects reset */
    reply = call_dbus_iterative(
        conn,
        "GetDNS",
        NULL,
        G_VARIANT_TYPE("(asb)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(asb)", &dns_iter, &is_custom);
    g_assert_false(is_custom);
    g_variant_iter_free(dns_iter);
    g_variant_unref(reply);

    /* Cleanup temp test dir */
    g_unsetenv("MULTIWIFI_DNS_CONF_DIR");
    g_unsetenv("MULTIWIFI_RESOLV_CONF");
    g_unlink(fake_resolv);
    g_free(fake_resolv);
    g_rmdir(tmp_dir);
    g_free(tmp_dir);

    g_object_unref(conn);
    multiwifi_daemon_service_stop(svc);
    g_object_unref(svc);
}

typedef struct {
    gboolean got_progress;
    gboolean got_finished;
    gchar *finished_op_id;
    gboolean finished_success;
} SignalTestCtx;

static void on_operation_progress_signal(GDBusConnection *connection,
                                        const gchar *sender_name,
                                        const gchar *object_path,
                                        const gchar *interface_name,
                                        const gchar *signal_name,
                                        GVariant *parameters,
                                        gpointer user_data)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    SignalTestCtx *ctx = (SignalTestCtx *)user_data;
    const gchar *op_id = NULL;
    guint32 percent = 0;
    const gchar *msg = NULL;
    g_variant_get(parameters, "(&su&s)", &op_id, &percent, &msg);
    if (op_id != NULL) {
        ctx->got_progress = TRUE;
    }
}

static void on_operation_finished_signal(GDBusConnection *connection,
                                        const gchar *sender_name,
                                        const gchar *object_path,
                                        const gchar *interface_name,
                                        const gchar *signal_name,
                                        GVariant *parameters,
                                        gpointer user_data)
{
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    SignalTestCtx *ctx = (SignalTestCtx *)user_data;
    const gchar *op_id = NULL;
    gboolean success = FALSE;
    GVariant *result = NULL;
    g_variant_get(parameters, "(&sb@a{sv})", &op_id, &success, &result);
    if (op_id != NULL) {
        ctx->got_finished = TRUE;
        ctx->finished_op_id = g_strdup(op_id);
        ctx->finished_success = success;
    }
    if (result != NULL) {
        g_variant_unref(result);
    }
}

static void test_daemon_dbus_signals(void)
{
    MultiWifiDaemonService *svc = multiwifi_daemon_service_new(G_BUS_TYPE_SESSION, FALSE);
    g_assert_nonnull(svc);

    GError *error = NULL;
    gboolean started = multiwifi_daemon_service_start(svc, &error);
    g_assert_no_error(error);
    g_assert_true(started);

    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(conn);

    GMainContext *ctx = g_main_context_default();
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(10000);
    }

    SignalTestCtx sig_ctx = {
        .got_progress = FALSE,
        .got_finished = FALSE,
        .finished_op_id = NULL,
        .finished_success = FALSE
    };

    guint sub_prog = g_dbus_connection_signal_subscribe(
        conn,
        MULTIWIFI_DBUS_NAME,
        MULTIWIFI_DBUS_INTERFACE,
        "OperationProgress",
        MULTIWIFI_DBUS_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_operation_progress_signal,
        &sig_ctx,
        NULL
    );
    g_assert_cmpuint(sub_prog, >, 0);

    guint sub_finish = g_dbus_connection_signal_subscribe(
        conn,
        MULTIWIFI_DBUS_NAME,
        MULTIWIFI_DBUS_INTERFACE,
        "OperationFinished",
        MULTIWIFI_DBUS_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_operation_finished_signal,
        &sig_ctx,
        NULL
    );
    g_assert_cmpuint(sub_finish, >, 0);

    /* Run diagnostics to trigger OperationProgress and OperationFinished signals */
    GVariant *reply = call_dbus_iterative(
        conn,
        "RunDiagnostics",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);

    const gchar *returned_op_id = NULL;
    g_variant_get(reply, "(&s)", &returned_op_id);
    g_assert_nonnull(returned_op_id);

    /* Wait for signals to be dispatched */
    gint64 deadline = g_get_monotonic_time() + 1000000; /* 1 second */
    while ((!sig_ctx.got_progress || !sig_ctx.got_finished) && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(ctx, TRUE);
    }

    g_assert_true(sig_ctx.got_progress);
    g_assert_true(sig_ctx.got_finished);
    g_assert_cmpstr(sig_ctx.finished_op_id, ==, returned_op_id);
    g_assert_true(sig_ctx.finished_success);

    g_free(sig_ctx.finished_op_id);
    g_variant_unref(reply);
    g_dbus_connection_signal_unsubscribe(conn, sub_prog);
    g_dbus_connection_signal_unsubscribe(conn, sub_finish);

    g_object_unref(conn);
    multiwifi_daemon_service_stop(svc);
    g_object_unref(svc);
}

static void test_daemon_polkit_denial(void)
{
    /* Start service with enable_polkit = TRUE on session bus */
    MultiWifiDaemonService *svc = multiwifi_daemon_service_new(G_BUS_TYPE_SESSION, TRUE);
    g_assert_nonnull(svc);

    GError *error = NULL;
    gboolean started = multiwifi_daemon_service_start(svc, &error);
    g_assert_no_error(error);
    g_assert_true(started);

    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(conn);

    GMainContext *ctx = g_main_context_default();
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(10000);
    }

    /* Read-only methods succeed without polkit */
    GVariant *reply = call_dbus_iterative(
        conn,
        "GetVersion",
        NULL,
        G_VARIANT_TYPE("(su)"),
        &error
    );
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    /* Mutating method StartLoadBalancing requires polkit: on session bus/unauthenticated, must fail */
    GVariantBuilder opt_b;
    g_variant_builder_init(&opt_b, G_VARIANT_TYPE("a{sv}"));
    reply = call_dbus_iterative(
        conn,
        "StartLoadBalancing",
        g_variant_new("(@a{sv})", g_variant_builder_end(&opt_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    gchar *remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_NOT_AUTHORIZED);
    g_free(remote_err);
    g_clear_error(&error);

    /* Mutating method StopDaemon also requires polkit */
    reply = call_dbus_iterative(
        conn,
        "StopDaemon",
        NULL,
        NULL,
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_NOT_AUTHORIZED);
    g_free(remote_err);
    g_clear_error(&error);

    /* Mutating method SetDNS requires polkit */
    GVariantBuilder polkit_dns_b;
    g_variant_builder_init(&polkit_dns_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&polkit_dns_b, "s", "1.1.1.1");
    reply = call_dbus_iterative(
        conn,
        "SetDNS",
        g_variant_new("(@as)", g_variant_builder_end(&polkit_dns_b)),
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_NOT_AUTHORIZED);
    g_free(remote_err);
    g_clear_error(&error);

    /* Mutating method ResetDNS requires polkit */
    reply = call_dbus_iterative(
        conn,
        "ResetDNS",
        NULL,
        G_VARIANT_TYPE("(s)"),
        &error
    );
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    remote_err = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_NOT_AUTHORIZED);
    g_free(remote_err);
    g_clear_error(&error);

    g_object_unref(conn);
    multiwifi_daemon_service_stop(svc);
    g_object_unref(svc);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/dbus/operation_manager_lifecycle", test_operation_manager_lifecycle);
    g_test_add_func("/dbus/operation_rollback_lifecycle", test_operation_rollback_lifecycle);
    g_test_add_func("/dbus/daemon_dbus_ipc_roundtrip", test_daemon_dbus_ipc_roundtrip);
    g_test_add_func("/dbus/daemon_dbus_signals", test_daemon_dbus_signals);
    g_test_add_func("/dbus/daemon_polkit_denial", test_daemon_polkit_denial);

    return g_test_run();
}
