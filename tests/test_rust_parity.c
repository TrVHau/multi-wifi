#define _POSIX_C_SOURCE 200809L

#include "multiwifi/core/config.h"
#include "multiwifi/core/journal.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_setup_pdeathsig(gpointer user_data)
{
    (void)user_data;
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

typedef struct {
    GPid pid;
    gint stdout_fd;
    gchar *address;
} PrivateDbus;

typedef struct {
    GPid pid;
} SpawnedDaemon;

typedef struct {
    gboolean got_progress;
    gboolean got_finished;
    gchar *finished_op_id;
    gboolean finished_success;
} SignalParityCtx;

static gboolean read_line_with_timeout(gint fd, gchar *buf, gsize buf_size, gint64 timeout_us)
{
    gsize pos = 0;
    gint64 deadline = g_get_monotonic_time() + timeout_us;

    while (pos + 1 < buf_size) {
        gint64 remaining_us = deadline - g_get_monotonic_time();
        if (remaining_us <= 0) {
            return FALSE;
        }
        int timeout_ms = (int)(remaining_us / 1000);
        if (timeout_ms < 1) {
            timeout_ms = 1;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (rc == 0) {
            return FALSE;
        }

        char ch = '\0';
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) {
            return FALSE;
        }
        if (ch == '\n') {
            buf[pos] = '\0';
            return TRUE;
        }
        buf[pos++] = ch;
    }

    buf[pos] = '\0';
    return FALSE;
}

static PrivateDbus *private_dbus_start(void)
{
    gchar *dbus_bin = g_find_program_in_path("dbus-daemon");
    g_assert_nonnull(dbus_bin);

    PrivateDbus *bus = g_new0(PrivateDbus, 1);
    bus->stdout_fd = -1;

    const gchar *argv[] = { dbus_bin, "--session", "--print-address=1", "--nofork", NULL };
    GError *err = NULL;
    gboolean ok = g_spawn_async_with_pipes(
        NULL,
        (gchar **)argv,
        NULL,
        G_SPAWN_DO_NOT_REAP_CHILD,
        child_setup_pdeathsig,
        NULL,
        &bus->pid,
        NULL,
        &bus->stdout_fd,
        NULL,
        &err
    );
    g_free(dbus_bin);
    g_assert_no_error(err);
    g_assert_true(ok);

    gchar line[512];
    gboolean got_addr = read_line_with_timeout(bus->stdout_fd, line, sizeof(line), 5000000);
    g_assert_true(got_addr);
    bus->address = g_strdup(g_strstrip(line));
    g_assert_true(*bus->address != '\0');
    return bus;
}

static void private_dbus_stop(PrivateDbus *bus)
{
    if (bus == NULL) {
        return;
    }
    if (bus->stdout_fd >= 0) {
        close(bus->stdout_fd);
        bus->stdout_fd = -1;
    }
    if (bus->pid > 0) {
        kill(bus->pid, SIGTERM);
        int status = 0;
        gint64 deadline = g_get_monotonic_time() + 3000000;
        while (waitpid(bus->pid, &status, WNOHANG) == 0) {
            if (g_get_monotonic_time() >= deadline) {
                kill(bus->pid, SIGKILL);
                waitpid(bus->pid, &status, 0);
                break;
            }
            g_usleep(10000);
        }
        g_spawn_close_pid(bus->pid);
        bus->pid = 0;
    }
    g_free(bus->address);
    g_free(bus);
}

static SpawnedDaemon *spawn_rust_daemon(const gchar *bus_address,
                                        gboolean enable_polkit,
                                        const gchar *config_path,
                                        const gchar *dns_dir,
                                        const gchar *resolv_conf)
{
    g_assert_true(g_file_test(MULTIWIFI_DAEMON_BIN, G_FILE_TEST_IS_EXECUTABLE));

    gchar **envp = g_get_environ();
    envp = g_environ_setenv(envp, "DBUS_SESSION_BUS_ADDRESS", bus_address, TRUE);
    if (config_path != NULL) {
        envp = g_environ_setenv(envp, "MULTIWIFI_CONFIG_PATH", config_path, TRUE);
    } else {
        envp = g_environ_unsetenv(envp, "MULTIWIFI_CONFIG_PATH");
    }
    if (dns_dir != NULL) {
        envp = g_environ_setenv(envp, "MULTIWIFI_DNS_CONF_DIR", dns_dir, TRUE);
    } else {
        envp = g_environ_unsetenv(envp, "MULTIWIFI_DNS_CONF_DIR");
    }
    if (resolv_conf != NULL) {
        envp = g_environ_setenv(envp, "MULTIWIFI_RESOLV_CONF", resolv_conf, TRUE);
    } else {
        envp = g_environ_unsetenv(envp, "MULTIWIFI_RESOLV_CONF");
    }

    const gchar *argv_no_polkit[] = { MULTIWIFI_DAEMON_BIN, "--session", "--no-polkit", NULL };
    const gchar *argv_polkit[] = { MULTIWIFI_DAEMON_BIN, "--session", NULL };

    SpawnedDaemon *d = g_new0(SpawnedDaemon, 1);
    GError *err = NULL;
    gboolean ok = g_spawn_async(
        NULL,
        (gchar **)(enable_polkit ? argv_polkit : argv_no_polkit),
        envp,
        G_SPAWN_DO_NOT_REAP_CHILD,
        child_setup_pdeathsig,
        NULL,
        &d->pid,
        &err
    );
    g_strfreev(envp);
    g_assert_no_error(err);
    g_assert_true(ok);
    return d;
}

static gboolean wait_daemon_exit(SpawnedDaemon *d, int *out_exit_code, gint64 timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    int status = 0;

    while (g_get_monotonic_time() < deadline) {
        pid_t w = waitpid(d->pid, &status, WNOHANG);
        if (w == d->pid) {
            g_spawn_close_pid(d->pid);
            d->pid = 0;
            if (out_exit_code != NULL) {
                *out_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            return TRUE;
        }
        g_usleep(10000);
    }
    return FALSE;
}

static void free_spawned_daemon(SpawnedDaemon *d)
{
    if (d == NULL) {
        return;
    }
    if (d->pid > 0) {
        kill(d->pid, SIGTERM);
        int code = 0;
        if (!wait_daemon_exit(d, &code, 2000000)) {
            kill(d->pid, SIGKILL);
            waitpid(d->pid, NULL, 0);
            g_spawn_close_pid(d->pid);
        }
        d->pid = 0;
    }
    g_free(d);
}

static GDBusConnection *connect_and_wait_daemon(const gchar *bus_address)
{
    GError *err = NULL;
    GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
        bus_address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,
        NULL,
        &err
    );
    g_assert_no_error(err);
    g_assert_nonnull(conn);

    gint64 deadline = g_get_monotonic_time() + 5000000;
    gboolean ready = FALSE;

    while (g_get_monotonic_time() < deadline) {
        GVariant *reply = g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            g_variant_new("(s)", MULTIWIFI_DBUS_NAME),
            G_VARIANT_TYPE("(b)"),
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            &err
        );
        if (reply != NULL) {
            g_variant_get(reply, "(b)", &ready);
            g_variant_unref(reply);
            if (ready) {
                break;
            }
        } else {
            g_clear_error(&err);
        }
        g_usleep(20000);
    }

    g_assert_true(ready);
    return conn;
}

static const GDBusArgInfo *find_arg(GDBusArgInfo **args, guint idx, const GDBusArgInfo *needle)
{
    (void)needle;
    if (args == NULL) {
        return NULL;
    }
    for (guint i = 0; args[i] != NULL; i++) {
        if (i == idx) {
            return args[i];
        }
    }
    return NULL;
}

static void assert_args_match(GDBusArgInfo **expected, GDBusArgInfo **actual, const gchar *member)
{
    guint exp_len = 0;
    guint act_len = 0;
    while (expected != NULL && expected[exp_len] != NULL) {
        exp_len++;
    }
    while (actual != NULL && actual[act_len] != NULL) {
        act_len++;
    }
    g_assert_cmpuint(act_len, ==, exp_len);

    for (guint i = 0; i < exp_len; i++) {
        const GDBusArgInfo *e = find_arg(expected, i, expected[i]);
        const GDBusArgInfo *a = find_arg(actual, i, actual[i]);
        g_assert_nonnull(e);
        g_assert_nonnull(a);
        if (g_strcmp0(e->signature, a->signature) != 0) {
            g_error("Signature mismatch on member '%s' arg %u ('%s' vs '%s')",
                    member, i, e->signature, a->signature);
        }
        g_assert_cmpstr(e->signature, ==, a->signature);
    }
}

static void test_introspection_and_read_contract(void)
{
    PrivateDbus *bus = private_dbus_start();
    SpawnedDaemon *daemon = spawn_rust_daemon(bus->address, FALSE, NULL, NULL, NULL);
    GDBusConnection *conn = connect_and_wait_daemon(bus->address);

    /* 1. Compare runtime Introspect XML against canonical data/io.github.multiwifi.Daemon1.xml */
    gchar *xml_path = g_build_filename(MULTIWIFI_SOURCE_DIR, "data", "io.github.multiwifi.Daemon1.xml", NULL);
    gchar *expected_xml = NULL;
    GError *err = NULL;
    g_assert_true(g_file_get_contents(xml_path, &expected_xml, NULL, &err));
    g_assert_no_error(err);

    GDBusNodeInfo *expected_node = g_dbus_node_info_new_for_xml(expected_xml, &err);
    g_assert_no_error(err);
    g_assert_nonnull(expected_node);
    GDBusInterfaceInfo *expected_iface = g_dbus_node_info_lookup_interface(expected_node, MULTIWIFI_DBUS_INTERFACE);
    g_assert_nonnull(expected_iface);

    GVariant *intro_reply = g_dbus_connection_call_sync(
        conn,
        MULTIWIFI_DBUS_NAME,
        MULTIWIFI_DBUS_PATH,
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &err
    );
    g_assert_no_error(err);
    const gchar *runtime_xml = NULL;
    g_variant_get(intro_reply, "(&s)", &runtime_xml);

    GDBusNodeInfo *runtime_node = g_dbus_node_info_new_for_xml(runtime_xml, &err);
    g_assert_no_error(err);
    g_assert_nonnull(runtime_node);
    GDBusInterfaceInfo *runtime_iface = g_dbus_node_info_lookup_interface(runtime_node, MULTIWIFI_DBUS_INTERFACE);
    g_assert_nonnull(runtime_iface);

    for (guint i = 0; expected_iface->methods[i] != NULL; i++) {
        const GDBusMethodInfo *em = expected_iface->methods[i];
        const GDBusMethodInfo *rm = g_dbus_interface_info_lookup_method(runtime_iface, em->name);
        g_assert_nonnull(rm);
        assert_args_match(em->in_args, rm->in_args, em->name);
        assert_args_match(em->out_args, rm->out_args, em->name);
    }
    for (guint i = 0; expected_iface->signals[i] != NULL; i++) {
        const GDBusSignalInfo *es = expected_iface->signals[i];
        const GDBusSignalInfo *rs = g_dbus_interface_info_lookup_signal(runtime_iface, es->name);
        g_assert_nonnull(rs);
        assert_args_match(es->args, rs->args, es->name);
    }

    g_variant_unref(intro_reply);
    g_dbus_node_info_unref(runtime_node);
    g_dbus_node_info_unref(expected_node);
    g_free(expected_xml);
    g_free(xml_path);

    /* 2. Verify GetVersion */
    GVariant *ver_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetVersion", NULL, G_VARIANT_TYPE("(su)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    const gchar *ver_str = NULL;
    guint32 api_ver = 0;
    g_variant_get(ver_reply, "(&su)", &ver_str, &api_ver);
    g_assert_cmpstr(ver_str, ==, MULTIWIFI_VERSION_STRING);
    g_assert_cmpuint(api_ver, ==, MULTIWIFI_API_VERSION);
    g_variant_unref(ver_reply);

    /* 3. Verify GetCapabilities schema expected by GUI */
    GVariant *caps_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetCapabilities", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    g_assert_no_error(err);
    GVariant *caps_dict = NULL;
    g_variant_get(caps_reply, "(@a{sv})", &caps_dict);
    const gchar *cap_keys[] = {
        "kernel_version", "mptcp_state", "mptcp_details",
        "nftables_state", "tun_state", "network_manager_state", NULL
    };
    for (int i = 0; cap_keys[i] != NULL; i++) {
        const gchar *val = NULL;
        g_assert_true(g_variant_lookup(caps_dict, cap_keys[i], "&s", &val));
        g_assert_true(*val != '\0');
    }
    g_variant_unref(caps_dict);
    g_variant_unref(caps_reply);

    /* 4. Verify GetStatus schema expected by GUI on_status_query_done() */
    GVariant *status_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    GVariant *status_dict = NULL;
    g_variant_get(status_reply, "(@a{sv})", &status_dict);

    const gchar *mode = NULL;
    const gchar *version = NULL;
    const gchar *bstate = NULL;
    const gchar *vip = NULL;
    gboolean daemon_running = FALSE;
    gboolean routing_active = TRUE;
    gboolean firewall_active = TRUE;
    gboolean bonding_active = TRUE;
    gboolean bonding_mptcp = TRUE;
    gboolean polkit_enforced = TRUE;
    guint32 eligible_uplinks = 99;

    g_assert_true(g_variant_lookup(status_dict, "version", "&s", &version));
    g_assert_cmpstr(version, ==, MULTIWIFI_VERSION_STRING);
    g_assert_true(g_variant_lookup(status_dict, "mode", "&s", &mode));
    g_assert_cmpstr(mode, ==, "idle");
    g_assert_true(g_variant_lookup(status_dict, "daemon_running", "b", &daemon_running));
    g_assert_true(daemon_running);
    g_assert_true(g_variant_lookup(status_dict, "routing_active", "b", &routing_active));
    g_assert_false(routing_active);
    g_assert_true(g_variant_lookup(status_dict, "firewall_active", "b", &firewall_active));
    g_assert_false(firewall_active);
    g_assert_true(g_variant_lookup(status_dict, "bonding_active", "b", &bonding_active));
    g_assert_false(bonding_active);
    g_assert_true(g_variant_lookup(status_dict, "bonding_state", "&s", &bstate));
    g_assert_true(g_variant_lookup(status_dict, "bonding_mptcp", "b", &bonding_mptcp));
    g_assert_true(g_variant_lookup(status_dict, "bonding_virtual_ip", "&s", &vip));
    g_assert_true(g_variant_lookup(status_dict, "eligible_uplinks", "u", &eligible_uplinks));
    g_assert_true(g_variant_lookup(status_dict, "polkit_enforced", "b", &polkit_enforced));
    g_assert_false(polkit_enforced);

    g_variant_unref(status_dict);
    g_variant_unref(status_reply);

    /* 5. Verify GetDevices & GetPaths signature */
    GVariant *dev_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetDevices", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(dev_reply);

    GVariant *path_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetPaths", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(path_reply);

    /* 6. Verify StopDaemon cleanly shuts down Rust daemon with exit code 0 */
    GVariant *stop_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StopDaemon", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(stop_reply);

    int exit_code = -1;
    g_assert_true(wait_daemon_exit(daemon, &exit_code, 5000000));
    g_assert_cmpint(exit_code, ==, 0);

    g_object_unref(conn);
    free_spawned_daemon(daemon);
    private_dbus_stop(bus);
}

static void on_op_progress(GDBusConnection *conn,
                           const gchar *sender,
                           const gchar *path,
                           const gchar *iface,
                           const gchar *sig,
                           GVariant *params,
                           gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)sig;
    SignalParityCtx *ctx = (SignalParityCtx *)user_data;
    const gchar *op_id = NULL;
    guint32 percent = 0;
    const gchar *msg = NULL;
    g_variant_get(params, "(&su&s)", &op_id, &percent, &msg);
    if (op_id != NULL && *op_id != '\0') {
        ctx->got_progress = TRUE;
    }
}

static void on_op_finished(GDBusConnection *conn,
                           const gchar *sender,
                           const gchar *path,
                           const gchar *iface,
                           const gchar *sig,
                           GVariant *params,
                           gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)sig;
    SignalParityCtx *ctx = (SignalParityCtx *)user_data;
    const gchar *op_id = NULL;
    gboolean success = FALSE;
    GVariant *result = NULL;
    g_variant_get(params, "(&sb@a{sv})", &op_id, &success, &result);
    if (op_id != NULL) {
        ctx->got_finished = TRUE;
        g_free(ctx->finished_op_id);
        ctx->finished_op_id = g_strdup(op_id);
        ctx->finished_success = success;
    }
    if (result != NULL) {
        g_variant_unref(result);
    }
}

static void assert_dbus_remote_error(GError **err, const gchar *expected_name)
{
    g_assert_nonnull(*err);
    g_assert_true(g_dbus_error_is_remote_error(*err));
    gchar *remote = g_dbus_error_get_remote_error(*err);
    g_assert_cmpstr(remote, ==, expected_name);
    g_free(remote);
    g_clear_error(err);
}

static void test_mutating_methods_signals_and_dns(void)
{
    GError *err = NULL;
    gchar *tmp_dir = g_dir_make_tmp("multiwifi_rust_parity_XXXXXX", &err);
    g_assert_no_error(err);

    gchar *resolv_path = g_build_filename(tmp_dir, "resolv.conf", NULL);
    g_assert_true(g_file_set_contents(resolv_path, "nameserver 192.168.1.1\n", -1, &err));
    g_assert_no_error(err);

    /* Generate multiwifi.conf via C serializer and feed it to Rust daemon */
    MultiWifiConfig *c_cfg = multiwifi_config_new_default();
    c_cfg->load_balance.health_check_interval_ms = 1500;
    gchar *cfg_toml = multiwifi_config_serialize(c_cfg);
    g_assert_nonnull(cfg_toml);
    gchar *cfg_path = g_build_filename(tmp_dir, "multiwifi.conf", NULL);
    g_assert_true(g_file_set_contents(cfg_path, cfg_toml, -1, &err));
    g_assert_no_error(err);
    g_free(cfg_toml);
    multiwifi_config_free(c_cfg);

    PrivateDbus *bus = private_dbus_start();
    SpawnedDaemon *daemon = spawn_rust_daemon(bus->address, FALSE, cfg_path, tmp_dir, resolv_path);
    GDBusConnection *conn = connect_and_wait_daemon(bus->address);

    /* 1. SetPathWeight validation & happy path */
    GVariant *reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetPathWeight", g_variant_new("(su)", "wlan0", 0), NULL,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_INVALID_ARGUMENT);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetPathWeight", g_variant_new("(su)", "", 100), NULL,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_INVALID_ARGUMENT);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetPathWeight", g_variant_new("(su)", "wlan0", 250), NULL,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    /* 2. StopLoadBalancing & StopBonding idempotent checks */
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StopLoadBalancing", NULL, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    /* StartBonding rejects invalid payload cleanly */
    GVariantBuilder bad_bond;
    g_variant_builder_init(&bad_bond, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&bad_bond, "{sv}", "server_host", g_variant_new_string("127.0.0.1"));
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StartBonding", g_variant_new("(@a{sv})", g_variant_builder_end(&bad_bond)),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_INVALID_ARGUMENT);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StopBonding", NULL, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    /* 3. RunDiagnostics + OperationProgress/OperationFinished signals + GetDiagnostics + rate limit */
    SignalParityCtx sig_ctx = { 0 };
    guint sub_prog = g_dbus_connection_signal_subscribe(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_INTERFACE, "OperationProgress",
        MULTIWIFI_DBUS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_op_progress, &sig_ctx, NULL
    );
    guint sub_fin = g_dbus_connection_signal_subscribe(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_INTERFACE, "OperationFinished",
        MULTIWIFI_DBUS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_op_finished, &sig_ctx, NULL
    );

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "RunDiagnostics", NULL, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    gchar *diag_op_id = NULL;
    g_variant_get(reply, "(s)", &diag_op_id);
    g_variant_unref(reply);

    GMainContext *ctx = g_main_context_default();
    gint64 deadline = g_get_monotonic_time() + 2000000;
    while ((!sig_ctx.got_progress || !sig_ctx.got_finished) && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(ctx, FALSE);
        g_usleep(10000);
    }
    g_assert_true(sig_ctx.got_progress);
    g_assert_true(sig_ctx.got_finished);
    g_assert_cmpstr(sig_ctx.finished_op_id, ==, diag_op_id);
    g_assert_true(sig_ctx.finished_success);

    g_dbus_connection_signal_unsubscribe(conn, sub_prog);
    g_dbus_connection_signal_unsubscribe(conn, sub_fin);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetDiagnostics", g_variant_new("(s)", diag_op_id), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    GVariant *diag_dict = NULL;
    g_variant_get(reply, "(@a{sv})", &diag_dict);
    const gchar *action = NULL;
    const gchar *op_state = NULL;
    guint32 pct = 0;
    gboolean op_ok = FALSE;
    g_assert_true(g_variant_lookup(diag_dict, "action", "&s", &action));
    g_assert_cmpstr(action, ==, "RunDiagnostics");
    g_assert_true(g_variant_lookup(diag_dict, "state", "&s", &op_state));
    g_assert_cmpstr(op_state, ==, "SUCCEEDED");
    g_assert_true(g_variant_lookup(diag_dict, "percent", "u", &pct));
    g_assert_cmpuint(pct, ==, 100);
    g_assert_true(g_variant_lookup(diag_dict, "success", "b", &op_ok));
    g_assert_true(op_ok);
    g_variant_unref(diag_dict);
    g_variant_unref(reply);
    g_free(diag_op_id);
    g_free(sig_ctx.finished_op_id);

    /* Immediate second RunDiagnostics must trigger Busy rate limit */
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "RunDiagnostics", NULL, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_BUSY);

    /* 4. DNS workflow & symlink hardening through GDBus */
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetDNS", NULL, G_VARIANT_TYPE("(asb)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    GVariantIter *iter = NULL;
    gboolean is_custom = TRUE;
    g_variant_get(reply, "(asb)", &iter, &is_custom);
    g_assert_false(is_custom);
    g_variant_iter_free(iter);
    g_variant_unref(reply);

    GVariantBuilder cf_dns;
    g_variant_builder_init(&cf_dns, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&cf_dns, "s", "1.1.1.1");
    g_variant_builder_add(&cf_dns, "s", "1.0.0.1");
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetDNS", g_variant_new("(@as)", g_variant_builder_end(&cf_dns)),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetDNS", NULL, G_VARIANT_TYPE("(asb)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_get(reply, "(asb)", &iter, &is_custom);
    g_assert_true(is_custom);
    const gchar *srv = NULL;
    gboolean saw_cf = FALSE;
    while (g_variant_iter_next(iter, "&s", &srv)) {
        if (g_strcmp0(srv, "1.1.1.1") == 0) {
            saw_cf = TRUE;
        }
    }
    g_assert_true(saw_cf);
    g_variant_iter_free(iter);
    g_variant_unref(reply);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "ResetDNS", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    /* Symlink hardening check */
    gchar *nm_conf = g_build_filename(tmp_dir, "99-multiwifi-dns.conf", NULL);
    gchar *victim = g_build_filename(tmp_dir, "victim.txt", NULL);
    g_assert_cmpint(symlink(victim, nm_conf), ==, 0);

    GVariantBuilder sym_dns;
    g_variant_builder_init(&sym_dns, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&sym_dns, "s", "8.8.8.8");
    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetDNS", g_variant_new("(@as)", g_variant_builder_end(&sym_dns)),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_NETWORK_FAILURE);
    g_assert_false(g_file_test(victim, G_FILE_TEST_EXISTS));

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StopDaemon", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    g_variant_unref(reply);

    int exit_code = -1;
    g_assert_true(wait_daemon_exit(daemon, &exit_code, 5000000));
    g_assert_cmpint(exit_code, ==, 0);

    g_object_unref(conn);
    free_spawned_daemon(daemon);
    private_dbus_stop(bus);

    g_unlink(nm_conf);
    g_unlink(cfg_path);
    g_unlink(resolv_path);
    g_rmdir(tmp_dir);
    g_free(nm_conf);
    g_free(victim);
    g_free(cfg_path);
    g_free(resolv_path);
    g_free(tmp_dir);
}

static void test_polkit_enforcement_and_sigterm(void)
{
    PrivateDbus *bus = private_dbus_start();
    SpawnedDaemon *daemon = spawn_rust_daemon(bus->address, TRUE, NULL, NULL, NULL);
    GDBusConnection *conn = connect_and_wait_daemon(bus->address);

    GError *err = NULL;
    GVariant *status_reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_no_error(err);
    GVariant *status_dict = NULL;
    g_variant_get(status_reply, "(@a{sv})", &status_dict);
    gboolean polkit_enforced = FALSE;
    g_assert_true(g_variant_lookup(status_dict, "polkit_enforced", "b", &polkit_enforced));
    g_assert_true(polkit_enforced);
    g_variant_unref(status_dict);
    g_variant_unref(status_reply);

    GVariantBuilder opt_b;
    g_variant_builder_init(&opt_b, G_VARIANT_TYPE("a{sv}"));
    GVariant *reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StartLoadBalancing", g_variant_new("(@a{sv})", g_variant_builder_end(&opt_b)),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_NOT_AUTHORIZED);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "SetPathWeight", g_variant_new("(su)", "wlan0", 200),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_NOT_AUTHORIZED);

    reply = g_dbus_connection_call_sync(
        conn, MULTIWIFI_DBUS_NAME, MULTIWIFI_DBUS_PATH, MULTIWIFI_DBUS_INTERFACE,
        "StopDaemon", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err
    );
    g_assert_null(reply);
    assert_dbus_remote_error(&err, MULTIWIFI_ERROR_NOT_AUTHORIZED);

    /* Terminate daemon with SIGTERM and confirm graceful 0 exit */
    kill(daemon->pid, SIGTERM);
    int exit_code = -1;
    g_assert_true(wait_daemon_exit(daemon, &exit_code, 5000000));
    g_assert_cmpint(exit_code, ==, 0);

    g_object_unref(conn);
    free_spawned_daemon(daemon);
    private_dbus_stop(bus);
}

static void test_config_and_journal_schema_parity(void)
{
    GError *err = NULL;
    gchar *tmp_dir = g_dir_make_tmp("multiwifi_schema_parity_XXXXXX", &err);
    g_assert_no_error(err);

    /* C-serialized journal must match Rust RouteJournal INI keys */
    gchar *journal_path = g_build_filename(tmp_dir, "op-parity.journal", NULL);
    MultiWifiRouteJournal *j = multiwifi_route_journal_new("op-parity");
    multiwifi_route_journal_append(
        j, multiwifi_journal_entry_new(MULTIWIFI_JOURNAL_OP_ADD_RULE, 10001, 21001, 0x1001, 0xff00, NULL, NULL, "wlan0", 0)
    );
    multiwifi_route_journal_append(
        j, multiwifi_journal_entry_new(MULTIWIFI_JOURNAL_OP_ADD_ROUTE, 10001, 0, 0, 0, NULL, "192.168.1.1", "wlan0", 100)
    );
    g_assert_true(multiwifi_route_journal_save_to_file(j, journal_path, &err));
    g_assert_no_error(err);
    multiwifi_route_journal_free(j);

    gchar *ini_text = NULL;
    g_assert_true(g_file_get_contents(journal_path, &ini_text, NULL, &err));
    g_assert_no_error(err);
    g_assert_nonnull(strstr(ini_text, "[journal]"));
    g_assert_nonnull(strstr(ini_text, "op_id=op-parity"));
    g_assert_nonnull(strstr(ini_text, "count=2"));
    g_assert_nonnull(strstr(ini_text, "[entry_0]"));
    g_assert_nonnull(strstr(ini_text, "table_id=10001"));
    g_assert_nonnull(strstr(ini_text, "[entry_1]"));
    g_assert_nonnull(strstr(ini_text, "gateway=192.168.1.1"));

    g_free(ini_text);
    g_unlink(journal_path);
    g_rmdir(tmp_dir);
    g_free(journal_path);
    g_free(tmp_dir);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/rust_parity/introspection_and_read_contract", test_introspection_and_read_contract);
    g_test_add_func("/rust_parity/mutating_methods_signals_and_dns", test_mutating_methods_signals_and_dns);
    g_test_add_func("/rust_parity/polkit_enforcement_and_sigterm", test_polkit_enforcement_and_sigterm);
    g_test_add_func("/rust_parity/config_and_journal_schema_parity", test_config_and_journal_schema_parity);

    return g_test_run();
}
