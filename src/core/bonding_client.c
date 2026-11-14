#define _GNU_SOURCE
#include "multiwifi/core/bonding_client.h"
#include <gio/gio.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#ifndef IPPROTO_MPTCP
#define IPPROTO_MPTCP 262
#endif

struct _MultiWifiBondingClient {
    gchar *profile_id;
    gchar *server_host;
    uint16_t server_port;
    gchar *auth_token;
    gchar *physical_ifname;
    gchar *physical_gw;

    MultiWifiBondingState state;
    gboolean is_mptcp;
    int sock_fd;
    gchar *virtual_ip;
    gchar *virtual_netmask;
    uint32_t server_mtu;

    MultiWifiTunDevice *tun;
    MultiWifiLoopPrevention *loop_prev;
    GThread *worker;
    gboolean running;
    GMutex mutex;
};

const gchar *multiwifi_bonding_client_state_to_string(MultiWifiBondingState state)
{
    switch (state) {
    case MULTIWIFI_BONDING_STATE_DISCONNECTED:
        return "Disconnected";
    case MULTIWIFI_BONDING_STATE_CONNECTING:
        return "Connecting";
    case MULTIWIFI_BONDING_STATE_AUTHENTICATING:
        return "Authenticating";
    case MULTIWIFI_BONDING_STATE_CONNECTED:
        return "Connected";
    case MULTIWIFI_BONDING_STATE_DISCONNECTING:
        return "Disconnecting";
    case MULTIWIFI_BONDING_STATE_FAILED:
        return "Failed";
    default:
        return "Unknown";
    }
}

MultiWifiBondingClient *multiwifi_bonding_client_new(void)
{
    MultiWifiBondingClient *self = g_new0(MultiWifiBondingClient, 1);
    self->state = MULTIWIFI_BONDING_STATE_DISCONNECTED;
    self->sock_fd = -1;
    self->is_mptcp = FALSE;
    self->running = FALSE;
    self->server_mtu = MULTIWIFI_DEFAULT_TUN_MTU;
    g_mutex_init(&self->mutex);
    return self;
}

void multiwifi_bonding_client_free(MultiWifiBondingClient *self)
{
    if (self == NULL) {
        return;
    }
    multiwifi_bonding_client_stop(self);
    g_mutex_clear(&self->mutex);

    g_free(self->profile_id);
    g_free(self->server_host);
    g_free(self->auth_token);
    g_free(self->physical_ifname);
    g_free(self->physical_gw);
    g_free(self->virtual_ip);
    g_free(self->virtual_netmask);

    if (self->tun != NULL) {
        multiwifi_tun_device_free(self->tun);
    }
    if (self->loop_prev != NULL) {
        multiwifi_loop_prevention_free(self->loop_prev);
    }

    g_free(self);
}

void multiwifi_bonding_client_set_profile(MultiWifiBondingClient *self,
                                          const gchar *profile_id,
                                          const gchar *server_host,
                                          uint16_t server_port,
                                          const gchar *auth_token)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    g_free(self->profile_id);
    self->profile_id = g_strdup(profile_id);
    g_free(self->server_host);
    self->server_host = g_strdup(server_host);
    self->server_port = server_port;
    g_free(self->auth_token);
    self->auth_token = g_strdup(auth_token);
    g_mutex_unlock(&self->mutex);
}

void multiwifi_bonding_client_set_uplink(MultiWifiBondingClient *self,
                                         const gchar *physical_ifname,
                                         const gchar *physical_gw)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    g_free(self->physical_ifname);
    self->physical_ifname = g_strdup(physical_ifname);
    g_free(self->physical_gw);
    self->physical_gw = g_strdup(physical_gw);
    g_mutex_unlock(&self->mutex);
}

void multiwifi_bonding_client_set_tun_device(MultiWifiBondingClient *self,
                                             MultiWifiTunDevice *tun)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    if (self->tun != NULL) {
        multiwifi_tun_device_free(self->tun);
    }
    self->tun = tun;
    g_mutex_unlock(&self->mutex);
}

static gpointer bonding_worker_thread(gpointer user_data)
{
    MultiWifiBondingClient *self = (MultiWifiBondingClient *)user_data;
    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    guint8 io_buf[65536];
    gint64 last_ping_us = g_get_monotonic_time();
    uint32_t seq = 10;

    int tun_fd = multiwifi_tun_device_get_fd(self->tun);
    int sock_fd = self->sock_fd;

    while (TRUE) {
        g_mutex_lock(&self->mutex);
        gboolean run = self->running;
        g_mutex_unlock(&self->mutex);

        if (!run) {
            break;
        }

        struct pollfd fds[2];
        fds[0].fd = tun_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = sock_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int poll_res = poll(fds, 2, 200);
        if (poll_res < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /* 1. TUN readable -> send packet to socket */
        if (fds[0].revents & POLLIN) {
            gssize n_read = multiwifi_tun_device_read(self->tun, io_buf, sizeof(io_buf), NULL);
            if (n_read > 0) {
                GByteArray *frame_bytes = multiwifi_frame_encode_ip(seq++, io_buf, (uint32_t)n_read);
                ssize_t sent = send(sock_fd, frame_bytes->data, frame_bytes->len, MSG_NOSIGNAL);
                (void)sent;
                g_byte_array_free(frame_bytes, TRUE);
            }
        }

        /* 2. Socket readable -> parse frame and forward to TUN */
        if (fds[1].revents & POLLIN) {
            ssize_t n_sock = recv(sock_fd, io_buf, sizeof(io_buf), 0);
            if (n_sock <= 0) {
                /* Server closed connection */
                break;
            }

            GList *frames = multiwifi_frame_parser_feed(parser, io_buf, (gsize)n_sock, NULL);
            for (GList *l = frames; l != NULL; l = l->next) {
                MultiWifiFrame *f = (MultiWifiFrame *)l->data;
                if (f->type == MULTIWIFI_FRAME_IP_PACKET && f->length > 0) {
                    multiwifi_tun_device_write(self->tun, f->payload, f->length, NULL);
                } else if (f->type == MULTIWIFI_FRAME_PING) {
                    /* Echo PONG */
                    uint64_t client_ts = 0;
                    if (f->length >= 8) {
                        memcpy(&client_ts, f->payload, 8);
                        client_ts = GUINT64_FROM_BE(client_ts);
                    }
                    GByteArray *pong = multiwifi_frame_encode_pong(seq++, client_ts, (uint64_t)g_get_monotonic_time());
                    send(sock_fd, pong->data, pong->len, MSG_NOSIGNAL);
                    g_byte_array_free(pong, TRUE);
                } else if (f->type == MULTIWIFI_FRAME_DISCONNECT) {
                    multiwifi_frame_free(f);
                    g_list_free(frames);
                    goto thread_exit;
                }
                multiwifi_frame_free(f);
            }
            g_list_free(frames);
        }

        /* 3. Keepalive ping every 3 seconds */
        gint64 now_us = g_get_monotonic_time();
        if (now_us - last_ping_us >= 3000000) {
            last_ping_us = now_us;
            GByteArray *ping = multiwifi_frame_encode_ping(seq++, (uint64_t)now_us);
            send(sock_fd, ping->data, ping->len, MSG_NOSIGNAL);
            g_byte_array_free(ping, TRUE);
        }
    }

thread_exit:
    multiwifi_frame_parser_free(parser);
    return NULL;
}

gboolean multiwifi_bonding_client_start(MultiWifiBondingClient *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Bonding client is NULL");
        return FALSE;
    }

    g_mutex_lock(&self->mutex);

    if (self->server_host == NULL || *self->server_host == '\0' || self->server_port == 0) {
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Server host and port must be specified");
        return FALSE;
    }

    self->state = MULTIWIFI_BONDING_STATE_CONNECTING;

    /* Resolve host */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", self->server_port);

    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(self->server_host, port_str, &hints, &res);
    if (gai_err != 0 || res == NULL) {
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to resolve server host '%s': %s", self->server_host, gai_strerror(gai_err));
        return FALSE;
    }

    char resolved_ip[INET_ADDRSTRLEN] = { 0 };
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, resolved_ip, sizeof(resolved_ip));

    /* Apply loop-prevention host route if physical uplink parameters configured */
    if (self->physical_ifname != NULL && self->physical_gw != NULL) {
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_free(self->loop_prev);
        }
        self->loop_prev = multiwifi_loop_prevention_new(
            resolved_ip, self->physical_gw, self->physical_ifname, 10
        );
        multiwifi_loop_prevention_apply(self->loop_prev, NULL);
    }

    /* Socket creation: try MPTCP first, fall back to TCP */
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP);
    if (sock >= 0) {
        self->is_mptcp = TRUE;
    } else {
        self->is_mptcp = FALSE;
        sock = socket(AF_INET, SOCK_STREAM, 0);
    }

    if (sock < 0) {
        freeaddrinfo(res);
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Socket creation failed: %s", g_strerror(errno));
        return FALSE;
    }

    /* Set 5-second handshake timeouts */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        int err = errno;
        close(sock);
        freeaddrinfo(res);
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
        }
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to connect to %s:%u: %s", resolved_ip, self->server_port, g_strerror(err));
        return FALSE;
    }
    freeaddrinfo(res);
    self->sock_fd = sock;

    /* Handshake */
    self->state = MULTIWIFI_BONDING_STATE_AUTHENTICATING;
    GByteArray *auth_req = multiwifi_frame_encode_auth_req(1, self->profile_id, self->auth_token);
    ssize_t sent = send(self->sock_fd, auth_req->data, auth_req->len, 0);
    g_byte_array_free(auth_req, TRUE);

    if (sent < 0) {
        close(self->sock_fd);
        self->sock_fd = -1;
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
        }
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to send auth request to server");
        return FALSE;
    }

    /* Wait for AUTH_RESP */
    MultiWifiFrameParser *parser = multiwifi_frame_parser_new();
    guint8 resp_buf[1024];
    MultiWifiFrame *resp_frame = NULL;

    while (TRUE) {
        ssize_t n = recv(self->sock_fd, resp_buf, sizeof(resp_buf), 0);
        if (n <= 0) {
            break;
        }
        GList *flist = multiwifi_frame_parser_feed(parser, resp_buf, (gsize)n, NULL);
        if (flist != NULL) {
            resp_frame = (MultiWifiFrame *)flist->data;
            g_list_free(flist);
            break;
        }
    }
    multiwifi_frame_parser_free(parser);

    if (resp_frame == NULL) {
        close(self->sock_fd);
        self->sock_fd = -1;
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
        }
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Timeout or no response to auth request");
        return FALSE;
    }

    uint32_t auth_status = 0;
    gchar *vip = NULL;
    gchar *netmask = NULL;
    uint32_t mtu = 0;
    gchar *msg = NULL;

    gboolean decoded = multiwifi_frame_decode_auth_resp(
        resp_frame, &auth_status, &vip, &netmask, &mtu, &msg, error
    );
    multiwifi_frame_free(resp_frame);

    if (!decoded) {
        close(self->sock_fd);
        self->sock_fd = -1;
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
        }
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    if (auth_status != MULTIWIFI_AUTH_STATUS_OK) {
        close(self->sock_fd);
        self->sock_fd = -1;
        if (self->loop_prev != NULL) {
            multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
        }
        self->state = MULTIWIFI_BONDING_STATE_FAILED;
        g_mutex_unlock(&self->mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Server rejected authentication (code=%u): %s",
                    auth_status, msg ? msg : "Unauthorized");
        g_free(vip);
        g_free(netmask);
        g_free(msg);
        return FALSE;
    }

    g_free(self->virtual_ip);
    self->virtual_ip = vip;
    g_free(self->virtual_netmask);
    self->virtual_netmask = netmask;
    self->server_mtu = (mtu > 0) ? mtu : MULTIWIFI_DEFAULT_TUN_MTU;
    g_free(msg);

    /* Open and configure TUN */
    if (self->tun == NULL) {
        self->tun = multiwifi_tun_device_new(NULL, self->server_mtu);
    }
    if (!multiwifi_tun_device_is_open(self->tun)) {
        if (!multiwifi_tun_device_open(self->tun, error)) {
            close(self->sock_fd);
            self->sock_fd = -1;
            if (self->loop_prev != NULL) {
                multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
            }
            self->state = MULTIWIFI_BONDING_STATE_FAILED;
            g_mutex_unlock(&self->mutex);
            return FALSE;
        }
    }

    multiwifi_tun_device_configure_ip(self->tun, self->virtual_ip, self->virtual_netmask, NULL);

    /* Set non-blocking I/O on socket for worker */
    int fflags = fcntl(self->sock_fd, F_GETFL, 0);
    fcntl(self->sock_fd, F_SETFL, fflags | O_NONBLOCK);

    self->running = TRUE;
    self->state = MULTIWIFI_BONDING_STATE_CONNECTED;
    self->worker = g_thread_new("bonding-worker", bonding_worker_thread, self);

    g_mutex_unlock(&self->mutex);
    return TRUE;
}

void multiwifi_bonding_client_stop(MultiWifiBondingClient *self)
{
    if (self == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);
    if (!self->running && self->state == MULTIWIFI_BONDING_STATE_DISCONNECTED) {
        g_mutex_unlock(&self->mutex);
        return;
    }

    self->state = MULTIWIFI_BONDING_STATE_DISCONNECTING;
    self->running = FALSE;

    if (self->sock_fd >= 0) {
        GByteArray *disc = multiwifi_frame_encode_disconnect(999, 0);
        send(self->sock_fd, disc->data, disc->len, MSG_NOSIGNAL);
        g_byte_array_free(disc, TRUE);

        shutdown(self->sock_fd, SHUT_RDWR);
    }

    GThread *w = self->worker;
    self->worker = NULL;
    g_mutex_unlock(&self->mutex);

    if (w != NULL) {
        g_thread_join(w);
    }

    g_mutex_lock(&self->mutex);
    if (self->sock_fd >= 0) {
        close(self->sock_fd);
        self->sock_fd = -1;
    }

    if (self->tun != NULL) {
        multiwifi_tun_device_close(self->tun);
    }

    if (self->loop_prev != NULL) {
        multiwifi_loop_prevention_rollback(self->loop_prev, NULL);
    }

    self->state = MULTIWIFI_BONDING_STATE_DISCONNECTED;
    g_mutex_unlock(&self->mutex);
}

MultiWifiBondingState multiwifi_bonding_client_get_state(MultiWifiBondingClient *self)
{
    if (self == NULL) {
        return MULTIWIFI_BONDING_STATE_DISCONNECTED;
    }
    MultiWifiBondingState st;
    g_mutex_lock(&self->mutex);
    st = self->state;
    g_mutex_unlock(&self->mutex);
    return st;
}

const gchar *multiwifi_bonding_client_get_virtual_ip(MultiWifiBondingClient *self)
{
    if (self == NULL) {
        return NULL;
    }
    return self->virtual_ip;
}

gboolean multiwifi_bonding_client_is_mptcp(MultiWifiBondingClient *self)
{
    if (self == NULL) {
        return FALSE;
    }
    gboolean m;
    g_mutex_lock(&self->mutex);
    m = self->is_mptcp;
    g_mutex_unlock(&self->mutex);
    return m;
}
