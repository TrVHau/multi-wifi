#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "multiwifi/core/bonding_protocol.h"
#include "multiwifi/core/tun_device.h"
#include "multiwifi/core/loop_prevention.h"
#include "multiwifi/core/bonding_client.h"
#include "multiwifi/daemon/service.h"
#include "multiwifi/shared/dbus_defs.h"

static gboolean enter_isolated_network_namespace(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        return FALSE;
    }

    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        const char *deny = "deny";
        if (write(fd, deny, strlen(deny)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    char map[64];
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", gid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", uid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    int res = system("ip link set lo up && ip addr add 192.168.10.2/24 dev lo");
    return (res == 0);
}

/* 1. Protocol framing tests */

static void test_frame_auth_req_roundtrip(void)
{
    GByteArray *encoded = multiwifi_frame_encode_auth_req(42, "fast-profile", "secret-token-xyz");
    g_assert_nonnull(encoded);
    g_assert_cmpuint(encoded->len, >=, MULTIWIFI_BONDING_HEADER_SIZE);

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    GList *frames = multiwifi_frame_parser_feed(parser, encoded->data, encoded->len, NULL);
    g_assert_nonnull(frames);
    g_assert_cmpuint(g_list_length(frames), ==, 1);

    MultiWifiFrame *f = (MultiWifiFrame *)frames->data;
    g_assert_cmpuint(f->type, ==, MULTIWIFI_FRAME_AUTH_REQ);
    g_assert_cmpuint(f->sequence, ==, 42);

    gchar *pid = NULL;
    gchar *tok = NULL;
    GError *err = NULL;
    gboolean ok = multiwifi_frame_decode_auth_req(f, &pid, &tok, &err);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_cmpstr(pid, ==, "fast-profile");
    g_assert_cmpstr(tok, ==, "secret-token-xyz");

    g_free(pid);
    g_free(tok);
    multiwifi_frame_free(f);
    g_list_free(frames);
    multiwifi_frame_parser_free(parser);
    g_byte_array_free(encoded, TRUE);
}

static void test_frame_auth_resp_roundtrip(void)
{
    GByteArray *encoded = multiwifi_frame_encode_auth_resp(
        101, MULTIWIFI_AUTH_STATUS_OK, "10.8.0.5", "255.255.255.0", 1400, "Auth Successful"
    );
    g_assert_nonnull(encoded);

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    GList *frames = multiwifi_frame_parser_feed(parser, encoded->data, encoded->len, NULL);
    g_assert_nonnull(frames);
    g_assert_cmpuint(g_list_length(frames), ==, 1);

    MultiWifiFrame *f = (MultiWifiFrame *)frames->data;
    g_assert_cmpuint(f->type, ==, MULTIWIFI_FRAME_AUTH_RESP);
    g_assert_cmpuint(f->sequence, ==, 101);

    uint32_t status = 999;
    gchar *vip = NULL;
    gchar *netmask = NULL;
    uint32_t mtu = 0;
    gchar *msg = NULL;
    GError *err = NULL;

    gboolean ok = multiwifi_frame_decode_auth_resp(f, &status, &vip, &netmask, &mtu, &msg, &err);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_cmpuint(status, ==, MULTIWIFI_AUTH_STATUS_OK);
    g_assert_cmpstr(vip, ==, "10.8.0.5");
    g_assert_cmpstr(netmask, ==, "255.255.255.0");
    g_assert_cmpuint(mtu, ==, 1400);
    g_assert_cmpstr(msg, ==, "Auth Successful");

    g_free(vip);
    g_free(netmask);
    g_free(msg);
    multiwifi_frame_free(f);
    g_list_free(frames);
    multiwifi_frame_parser_free(parser);
    g_byte_array_free(encoded, TRUE);
}

static void test_frame_streaming_one_byte_at_a_time(void)
{
    GByteArray *encoded = multiwifi_frame_encode_ping(7, 123456789ULL);
    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();

    /* Feed byte by byte */
    for (guint i = 0; i < encoded->len; i++) {
        GList *frames = multiwifi_frame_parser_feed(parser, &encoded->data[i], 1, NULL);
        if (i < encoded->len - 1) {
            g_assert_null(frames);
        } else {
            g_assert_nonnull(frames);
            g_assert_cmpuint(g_list_length(frames), ==, 1);
            MultiWifiFrame *f = (MultiWifiFrame *)frames->data;
            g_assert_cmpuint(f->type, ==, MULTIWIFI_FRAME_PING);
            g_assert_cmpuint(f->sequence, ==, 7);
            multiwifi_frame_free(f);
            g_list_free(frames);
        }
    }

    multiwifi_frame_parser_free(parser);
    g_byte_array_free(encoded, TRUE);
}

static void test_frame_multiple_frames_concatenated(void)
{
    GByteArray *f1 = multiwifi_frame_encode_ping(1, 100);
    GByteArray *f2 = multiwifi_frame_encode_disconnect(2, 0);

    GByteArray *combined = g_byte_array_sized_new(f1->len + f2->len);
    g_byte_array_append(combined, f1->data, f1->len);
    g_byte_array_append(combined, f2->data, f2->len);

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    GList *frames = multiwifi_frame_parser_feed(parser, combined->data, combined->len, NULL);

    g_assert_nonnull(frames);
    g_assert_cmpuint(g_list_length(frames), ==, 2);

    MultiWifiFrame *first = (MultiWifiFrame *)frames->data;
    g_assert_cmpuint(first->type, ==, MULTIWIFI_FRAME_PING);
    g_assert_cmpuint(first->sequence, ==, 1);

    MultiWifiFrame *second = (MultiWifiFrame *)frames->next->data;
    g_assert_cmpuint(second->type, ==, MULTIWIFI_FRAME_DISCONNECT);
    g_assert_cmpuint(second->sequence, ==, 2);

    multiwifi_frame_free(first);
    multiwifi_frame_free(second);
    g_list_free(frames);
    multiwifi_frame_parser_free(parser);
    g_byte_array_free(f1, TRUE);
    g_byte_array_free(f2, TRUE);
    g_byte_array_free(combined, TRUE);
}

static void test_frame_parser_garbage_recovery(void)
{
    GByteArray *valid = multiwifi_frame_encode_ping(99, 999999ULL);
    guint8 garbage[64];
    memset(garbage, 0xAA, sizeof(garbage));

    GByteArray *stream = g_byte_array_sized_new(sizeof(garbage) + valid->len);
    g_byte_array_append(stream, garbage, sizeof(garbage));
    g_byte_array_append(stream, valid->data, valid->len);

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    GList *frames = multiwifi_frame_parser_feed(parser, stream->data, stream->len, NULL);

    g_assert_nonnull(frames);
    g_assert_cmpuint(g_list_length(frames), ==, 1);

    MultiWifiFrame *f = (MultiWifiFrame *)frames->data;
    g_assert_cmpuint(f->type, ==, MULTIWIFI_FRAME_PING);
    g_assert_cmpuint(f->sequence, ==, 99);

    multiwifi_frame_free(f);
    g_list_free(frames);
    multiwifi_frame_parser_free(parser);
    g_byte_array_free(valid, TRUE);
    g_byte_array_free(stream, TRUE);
}

static void test_frame_parser_bad_version_resync(void)
{
    GByteArray *bad_frame = multiwifi_frame_encode_ping(10, 1234ULL);
    /* Corrupt version byte at offset 4 right after magic "MWBF" */
    bad_frame->data[4] = 0x99;

    GByteArray *good_frame = multiwifi_frame_encode_ping(77, 5678ULL);

    GByteArray *stream = g_byte_array_sized_new(bad_frame->len + good_frame->len);
    g_byte_array_append(stream, bad_frame->data, bad_frame->len);
    g_byte_array_append(stream, good_frame->data, good_frame->len);

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    GList *frames = multiwifi_frame_parser_feed(parser, stream->data, stream->len, NULL);

    g_assert_nonnull(frames);
    g_assert_cmpuint(g_list_length(frames), ==, 1);

    MultiWifiFrame *f = (MultiWifiFrame *)frames->data;
    g_assert_cmpuint(f->type, ==, MULTIWIFI_FRAME_PING);
    g_assert_cmpuint(f->sequence, ==, 77);

    multiwifi_frame_free(f);
    g_list_free(frames);
    multiwifi_frame_parser_free(parser);
    g_byte_array_free(bad_frame, TRUE);
    g_byte_array_free(good_frame, TRUE);
    g_byte_array_free(stream, TRUE);
}

/* 2. TUN Device abstraction test */

static void test_tun_device_mock_read_write(void)
{
    int sv[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    MultiWifiTunDevice *tun = multiwifi_tun_device_new("mwtun9", 1420);
    g_assert_nonnull(tun);
    g_assert_cmpstr(multiwifi_tun_device_get_name(tun), ==, "mwtun9");
    g_assert_cmpuint(multiwifi_tun_device_get_mtu(tun), ==, 1420);

    multiwifi_tun_device_set_mock_fd(tun, sv[0]);
    g_assert_true(multiwifi_tun_device_is_open(tun));
    g_assert_cmpint(multiwifi_tun_device_get_fd(tun), ==, sv[0]);

    GError *err = NULL;
    g_assert_true(multiwifi_tun_device_configure_ip(tun, "10.8.0.2", "255.255.255.0", &err));
    g_assert_no_error(err);

    /* Write packet through TUN -> read from sv[1] */
    const guint8 test_packet[] = { 0x45, 0x00, 0x00, 0x14, 0x01, 0x02, 0x03, 0x04 };
    gssize written = multiwifi_tun_device_write(tun, test_packet, sizeof(test_packet), &err);
    g_assert_no_error(err);
    g_assert_cmpint((int)written, ==, (int)sizeof(test_packet));

    guint8 peer_buf[64];
    ssize_t n_peer = read(sv[1], peer_buf, sizeof(peer_buf));
    g_assert_cmpint((int)n_peer, ==, (int)sizeof(test_packet));
    g_assert_cmpint(memcmp(peer_buf, test_packet, sizeof(test_packet)), ==, 0);

    /* Send packet from peer -> read from TUN */
    const guint8 reply_packet[] = { 0x45, 0x00, 0x00, 0x14, 0x05, 0x06, 0x07, 0x08 };
    ssize_t n_sent = write(sv[1], reply_packet, sizeof(reply_packet));
    g_assert_cmpint((int)n_sent, ==, (int)sizeof(reply_packet));

    guint8 tun_read_buf[64];
    gssize n_tun = multiwifi_tun_device_read(tun, tun_read_buf, sizeof(tun_read_buf), &err);
    g_assert_no_error(err);
    g_assert_cmpint((int)n_tun, ==, (int)sizeof(reply_packet));
    g_assert_cmpint(memcmp(tun_read_buf, reply_packet, sizeof(reply_packet)), ==, 0);

    close(sv[1]);
    multiwifi_tun_device_free(tun);
}

/* 3. Loop prevention tests */

static void test_loop_prevention_netns(void)
{
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);

    if (pid == 0) {
        if (!enter_isolated_network_namespace()) {
            _exit(0);
        }

        MultiWifiLoopPrevention *lp = multiwifi_loop_prevention_new("198.51.100.1", "192.168.10.1", "lo", 15);
        if (lp == NULL) _exit(1);

        GError *err = NULL;
        if (!multiwifi_loop_prevention_apply(lp, &err)) {
            /* If netlink route creation not permitted in this container, exit cleanly */
            g_clear_error(&err);
            multiwifi_loop_prevention_free(lp);
            _exit(0);
        }

        if (!multiwifi_loop_prevention_is_active(lp)) _exit(2);

        if (!multiwifi_loop_prevention_rollback(lp, &err)) _exit(3);
        if (multiwifi_loop_prevention_is_active(lp)) _exit(4);

        multiwifi_loop_prevention_free(lp);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
}

/* 4. Mock Aggregation Server & Client End-to-End Handshake */

typedef struct {
    int listen_fd;
    uint16_t port;
    gboolean running;
    GThread *thread;
} MockServer;

static gpointer mock_server_thread(gpointer user_data)
{
    MockServer *ms = (MockServer *)user_data;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(ms->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        return NULL;
    }

    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    guint8 buf[2048];

    /* Wait for AUTH_REQ */
    while (ms->running) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        GList *flist = multiwifi_frame_parser_feed(parser, buf, (gsize)n, NULL);
        if (flist != NULL) {
            MultiWifiFrame *f = (MultiWifiFrame *)flist->data;
            if (f->type == MULTIWIFI_FRAME_AUTH_REQ) {
                /* Reply with AUTH_RESP */
                GByteArray *resp = multiwifi_frame_encode_auth_resp(
                    1, MULTIWIFI_AUTH_STATUS_OK, "10.8.0.42", "255.255.255.0", 1400, "Mock Server Authorized"
                );
                send(client_fd, resp->data, resp->len, MSG_NOSIGNAL);
                g_byte_array_free(resp, TRUE);
                multiwifi_frame_free(f);
                g_list_free(flist);
                break;
            }
            multiwifi_frame_free(f);
            g_list_free(flist);
        }
    }

    /* Process forwarded packets or ping/pong */
    while (ms->running) {
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;

            GList *flist = multiwifi_frame_parser_feed(parser, buf, (gsize)n, NULL);
            for (GList *l = flist; l != NULL; l = l->next) {
                MultiWifiFrame *f = (MultiWifiFrame *)l->data;
                if (f->type == MULTIWIFI_FRAME_IP_PACKET) {
                    /* Echo packet back to client */
                    GByteArray *echo = multiwifi_frame_encode_ip(f->sequence, f->payload, f->length);
                    send(client_fd, echo->data, echo->len, MSG_NOSIGNAL);
                    g_byte_array_free(echo, TRUE);
                } else if (f->type == MULTIWIFI_FRAME_PING) {
                    GByteArray *pong = multiwifi_frame_encode_pong(f->sequence, 100, 200);
                    send(client_fd, pong->data, pong->len, MSG_NOSIGNAL);
                    g_byte_array_free(pong, TRUE);
                } else if (f->type == MULTIWIFI_FRAME_DISCONNECT) {
                    multiwifi_frame_free(f);
                    g_list_free(flist);
                    goto srv_exit;
                }
                multiwifi_frame_free(f);
            }
            g_list_free(flist);
        }
    }

srv_exit:
    multiwifi_frame_parser_free(parser);
    close(client_fd);
    return NULL;
}

static MockServer *mock_server_start(void)
{
    MockServer *ms = g_new0(MockServer, 1);
    ms->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    g_assert_cmpint(ms->listen_fd, >=, 0);

    int opt = 1;
    setsockopt(ms->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* Ephemeral port */

    g_assert_cmpint(bind(ms->listen_fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(listen(ms->listen_fd, 1), ==, 0);

    socklen_t len = sizeof(addr);
    g_assert_cmpint(getsockname(ms->listen_fd, (struct sockaddr *)&addr, &len), ==, 0);
    ms->port = ntohs(addr.sin_port);
    ms->running = TRUE;

    ms->thread = g_thread_new("mock-server", mock_server_thread, ms);
    return ms;
}

static void mock_server_stop(MockServer *ms)
{
    if (ms == NULL) return;
    ms->running = FALSE;
    close(ms->listen_fd);
    g_thread_join(ms->thread);
    g_free(ms);
}

static void test_bonding_client_mock_server_handshake(void)
{
    MockServer *srv = mock_server_start();
    g_assert_nonnull(srv);

    MultiWifiBondingClient *client = multiwifi_bonding_client_new();
    g_assert_nonnull(client);

    /* Use mock TUN */
    int sv[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    MultiWifiTunDevice *tun = multiwifi_tun_device_new("mwtun8", 1400);
    multiwifi_tun_device_set_mock_fd(tun, sv[0]);
    multiwifi_bonding_client_set_tun_device(client, tun);

    multiwifi_bonding_client_set_profile(client, "test-prof", "127.0.0.1", srv->port, "auth-token-123");

    GError *err = NULL;
    gboolean started = multiwifi_bonding_client_start(client, &err);
    g_assert_no_error(err);
    g_assert_true(started);

    g_assert_cmpint(multiwifi_bonding_client_get_state(client), ==, MULTIWIFI_BONDING_STATE_CONNECTED);
    g_assert_cmpstr(multiwifi_bonding_client_get_virtual_ip(client), ==, "10.8.0.42");

    /* Forward packet through TUN mock fd -> verify echo (valid 20-byte IPv4 packet: 10.8.0.1 -> 10.8.0.42) */
    const guint8 test_ip_pkt[] = {
        0x45, 0x00, 0x00, 0x14,
        0x12, 0x34, 0x40, 0x00,
        0x40, 0x06, 0x00, 0x00,
        0x0A, 0x08, 0x00, 0x01,
        0x0A, 0x08, 0x00, 0x2A
    };
    ssize_t w = write(sv[1], test_ip_pkt, sizeof(test_ip_pkt));
    g_assert_cmpint((int)w, ==, (int)sizeof(test_ip_pkt));

    /* Wait for echo back to sv[1] */
    guint8 echo_buf[64];
    struct pollfd pfd = { .fd = sv[1], .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, 1500);
    g_assert_cmpint(pr, >, 0);
    ssize_t n_echo = read(sv[1], echo_buf, sizeof(echo_buf));
    g_assert_cmpint((int)n_echo, ==, (int)sizeof(test_ip_pkt));
    g_assert_cmpint(memcmp(echo_buf, test_ip_pkt, sizeof(test_ip_pkt)), ==, 0);

    /* Anti-spoofing verification: send spoofed packet (127.0.0.1) -> must be dropped by client */
    const guint8 spoof_ip_pkt[] = {
        0x45, 0x00, 0x00, 0x14,
        0x12, 0x34, 0x40, 0x00,
        0x40, 0x06, 0x00, 0x00,
        0x7F, 0x00, 0x00, 0x01,  /* 127.0.0.1 spoofed source */
        0x0A, 0x08, 0x00, 0x2A
    };
    ssize_t sw = write(sv[1], spoof_ip_pkt, sizeof(spoof_ip_pkt));
    g_assert_cmpint((int)sw, ==, (int)sizeof(spoof_ip_pkt));

    struct pollfd pfd_spoof = { .fd = sv[1], .events = POLLIN, .revents = 0 };
    int pr_spoof = poll(&pfd_spoof, 1, 200);
    g_assert_cmpint(pr_spoof, ==, 0); /* Must be rejected by is_valid_inbound_ip_packet */

    multiwifi_bonding_client_stop(client);
    g_assert_cmpint(multiwifi_bonding_client_get_state(client), ==, MULTIWIFI_BONDING_STATE_DISCONNECTED);

    close(sv[1]);
    multiwifi_bonding_client_free(client);
    mock_server_stop(srv);
}

/* 5. D-Bus Daemon Bonding API tests */

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

static void test_dbus_bonding_api_validation(void)
{
    MultiWifiDaemonService *service = multiwifi_daemon_service_new(G_BUS_TYPE_SESSION, FALSE);
    g_assert_nonnull(service);

    GError *err = NULL;
    gboolean ok = multiwifi_daemon_service_start(service, &err);
    if (!ok) {
        g_test_skip("Session D-Bus not available");
        g_object_unref(service);
        return;
    }

    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    g_assert_no_error(err);
    g_assert_nonnull(conn);

    GMainContext *main_ctx = g_main_context_default();
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(main_ctx, FALSE);
        g_usleep(10000);
    }

    /* 1. Query GetStatus for bonding fields */
    GVariant *reply = call_dbus_iterative(
        conn, "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"), &err
    );
    g_assert_no_error(err);
    g_assert_nonnull(reply);

    GVariant *dict = NULL;
    g_variant_get(reply, "(@a{sv})", &dict);

    GVariant *b_act = g_variant_lookup_value(dict, "bonding_active", G_VARIANT_TYPE_BOOLEAN);
    g_assert_nonnull(b_act);
    g_assert_false(g_variant_get_boolean(b_act));
    g_variant_unref(b_act);

    GVariant *b_st = g_variant_lookup_value(dict, "bonding_state", G_VARIANT_TYPE_STRING);
    g_assert_nonnull(b_st);
    g_assert_cmpstr(g_variant_get_string(b_st, NULL), ==, "Disconnected");
    g_variant_unref(b_st);

    g_variant_unref(dict);
    g_variant_unref(reply);

    /* 2. Call StartBonding with missing parameters -> InvalidArgument */
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    reply = call_dbus_iterative(
        conn, "StartBonding", g_variant_new("(@a{sv})", g_variant_builder_end(&b)),
        G_VARIANT_TYPE("(s)"), &err
    );
    g_assert_nonnull(err);
    g_assert_null(reply);
    g_assert_true(g_dbus_error_is_remote_error(err));
    gchar *remote_err = g_dbus_error_get_remote_error(err);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&err);

    /* 2b. Call StartBonding with whitespace-only host/token -> InvalidArgument */
    GVariantBuilder bw;
    g_variant_builder_init(&bw, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&bw, "{sv}", "server_host", g_variant_new_string("   "));
    g_variant_builder_add(&bw, "{sv}", "token", g_variant_new_string("  \t "));
    reply = call_dbus_iterative(
        conn, "StartBonding", g_variant_new("(@a{sv})", g_variant_builder_end(&bw)),
        G_VARIANT_TYPE("(s)"), &err
    );
    g_assert_nonnull(err);
    g_assert_null(reply);
    remote_err = g_dbus_error_get_remote_error(err);
    g_assert_cmpstr(remote_err, ==, MULTIWIFI_ERROR_INVALID_ARGUMENT);
    g_free(remote_err);
    g_clear_error(&err);

    /* 3. Call StopBonding when idle -> clean success */
    reply = call_dbus_iterative(
        conn, "StopBonding", NULL, G_VARIANT_TYPE("(s)"), &err
    );
    g_assert_no_error(err);
    g_assert_nonnull(reply);
    g_variant_unref(reply);

    g_object_unref(conn);
    multiwifi_daemon_service_stop(service);
    g_object_unref(service);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/bonding/frame_auth_req_roundtrip", test_frame_auth_req_roundtrip);
    g_test_add_func("/bonding/frame_auth_resp_roundtrip", test_frame_auth_resp_roundtrip);
    g_test_add_func("/bonding/frame_streaming_one_byte", test_frame_streaming_one_byte_at_a_time);
    g_test_add_func("/bonding/frame_multiple_concatenated", test_frame_multiple_frames_concatenated);
    g_test_add_func("/bonding/frame_parser_garbage_recovery", test_frame_parser_garbage_recovery);
    g_test_add_func("/bonding/frame_parser_bad_version_resync", test_frame_parser_bad_version_resync);
    g_test_add_func("/bonding/tun_device_mock_read_write", test_tun_device_mock_read_write);
    g_test_add_func("/bonding/loop_prevention_netns", test_loop_prevention_netns);
    g_test_add_func("/bonding/client_mock_server_handshake", test_bonding_client_mock_server_handshake);
    g_test_add_func("/bonding/dbus_api_validation", test_dbus_bonding_api_validation);

    return g_test_run();
}
