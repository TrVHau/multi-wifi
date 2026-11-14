#include "multiwifi/core/bonding_protocol.h"
#include <gio/gio.h>
#include <arpa/inet.h>
#include <string.h>

MultiWifiFrame *multiwifi_frame_new(void)
{
    return g_new0(MultiWifiFrame, 1);
}

void multiwifi_frame_free(MultiWifiFrame *frame)
{
    if (frame == NULL) {
        return;
    }
    g_free(frame->payload);
    g_free(frame);
}

GByteArray *multiwifi_frame_encode(uint8_t type,
                                   uint16_t flags,
                                   uint32_t sequence,
                                   const guint8 *payload,
                                   uint32_t payload_len)
{
    GByteArray *ba = g_byte_array_sized_new(MULTIWIFI_BONDING_HEADER_SIZE + payload_len);
    uint32_t magic_be = GUINT32_TO_BE(MULTIWIFI_BONDING_MAGIC);
    uint8_t ver = MULTIWIFI_BONDING_VERSION;
    uint16_t flags_be = GUINT16_TO_BE(flags);
    uint32_t len_be = GUINT32_TO_BE(payload_len);
    uint32_t seq_be = GUINT32_TO_BE(sequence);

    g_byte_array_append(ba, (const guint8 *)&magic_be, 4);
    g_byte_array_append(ba, &ver, 1);
    g_byte_array_append(ba, &type, 1);
    g_byte_array_append(ba, (const guint8 *)&flags_be, 2);
    g_byte_array_append(ba, (const guint8 *)&len_be, 4);
    g_byte_array_append(ba, (const guint8 *)&seq_be, 4);

    if (payload != NULL && payload_len > 0) {
        g_byte_array_append(ba, payload, payload_len);
    }

    return ba;
}

GByteArray *multiwifi_frame_encode_auth_req(uint32_t sequence,
                                            const gchar *profile_id,
                                            const gchar *token)
{
    const gchar *pid = profile_id ? profile_id : "";
    const gchar *tok = token ? token : "";
    uint16_t pid_len = (uint16_t)strlen(pid);
    uint16_t tok_len = (uint16_t)strlen(tok);

    uint32_t payload_size = 4 + 2 + pid_len + 2 + tok_len;
    guint8 *buf = g_malloc0(payload_size);

    uint32_t ver_be = GUINT32_TO_BE(MULTIWIFI_BONDING_VERSION);
    uint16_t pid_len_be = GUINT16_TO_BE(pid_len);
    uint16_t tok_len_be = GUINT16_TO_BE(tok_len);

    size_t offset = 0;
    memcpy(buf + offset, &ver_be, 4);
    offset += 4;
    memcpy(buf + offset, &pid_len_be, 2);
    offset += 2;
    if (pid_len > 0) {
        memcpy(buf + offset, pid, pid_len);
        offset += pid_len;
    }
    memcpy(buf + offset, &tok_len_be, 2);
    offset += 2;
    if (tok_len > 0) {
        memcpy(buf + offset, tok, tok_len);
        offset += tok_len;
    }

    GByteArray *ret = multiwifi_frame_encode(MULTIWIFI_FRAME_AUTH_REQ, 0, sequence, buf, payload_size);
    g_free(buf);
    return ret;
}

gboolean multiwifi_frame_decode_auth_req(const MultiWifiFrame *frame,
                                         gchar **out_profile_id,
                                         gchar **out_token,
                                         GError **error)
{
    if (frame == NULL || out_profile_id == NULL || out_token == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid parameters for decode");
        return FALSE;
    }

    if (frame->type != MULTIWIFI_FRAME_AUTH_REQ) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Frame is not AUTH_REQ");
        return FALSE;
    }

    if (frame->length < 8) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AUTH_REQ frame payload too short");
        return FALSE;
    }

    const guint8 *p = frame->payload;
    size_t offset = 4; /* skip client_version */

    uint16_t pid_len_be;
    memcpy(&pid_len_be, p + offset, 2);
    uint16_t pid_len = GUINT16_FROM_BE(pid_len_be);
    offset += 2;

    if (offset + pid_len + 2 > frame->length) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed profile_id in AUTH_REQ");
        return FALSE;
    }

    *out_profile_id = g_strndup((const gchar *)(p + offset), pid_len);
    offset += pid_len;

    uint16_t tok_len_be;
    memcpy(&tok_len_be, p + offset, 2);
    uint16_t tok_len = GUINT16_FROM_BE(tok_len_be);
    offset += 2;

    if (offset + tok_len > frame->length) {
        g_free(*out_profile_id);
        *out_profile_id = NULL;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed token in AUTH_REQ");
        return FALSE;
    }

    *out_token = g_strndup((const gchar *)(p + offset), tok_len);
    return TRUE;
}

GByteArray *multiwifi_frame_encode_auth_resp(uint32_t sequence,
                                             uint32_t status,
                                             const gchar *assigned_vip,
                                             const gchar *netmask,
                                             uint32_t server_mtu,
                                             const gchar *message)
{
    const gchar *msg = message ? message : "";
    uint16_t msg_len = (uint16_t)strlen(msg);

    uint32_t payload_size = 4 + 4 + 4 + 4 + 2 + msg_len;
    guint8 *buf = g_malloc0(payload_size);

    uint32_t status_be = GUINT32_TO_BE(status);
    uint32_t vip_raw = 0;
    if (assigned_vip != NULL && *assigned_vip != '\0') {
        inet_pton(AF_INET, assigned_vip, &vip_raw);
    }
    uint32_t mask_raw = 0;
    if (netmask != NULL && *netmask != '\0') {
        inet_pton(AF_INET, netmask, &mask_raw);
    }
    uint32_t mtu_be = GUINT32_TO_BE(server_mtu);
    uint16_t msg_len_be = GUINT16_TO_BE(msg_len);

    size_t offset = 0;
    memcpy(buf + offset, &status_be, 4);
    offset += 4;
    memcpy(buf + offset, &vip_raw, 4);
    offset += 4;
    memcpy(buf + offset, &mask_raw, 4);
    offset += 4;
    memcpy(buf + offset, &mtu_be, 4);
    offset += 4;
    memcpy(buf + offset, &msg_len_be, 2);
    offset += 2;
    if (msg_len > 0) {
        memcpy(buf + offset, msg, msg_len);
        offset += msg_len;
    }

    GByteArray *ret = multiwifi_frame_encode(MULTIWIFI_FRAME_AUTH_RESP, 0, sequence, buf, payload_size);
    g_free(buf);
    return ret;
}

gboolean multiwifi_frame_decode_auth_resp(const MultiWifiFrame *frame,
                                          uint32_t *out_status,
                                          gchar **out_vip,
                                          gchar **out_netmask,
                                          uint32_t *out_mtu,
                                          gchar **out_msg,
                                          GError **error)
{
    if (frame == NULL || out_status == NULL || out_vip == NULL ||
        out_netmask == NULL || out_mtu == NULL || out_msg == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments for decode");
        return FALSE;
    }

    if (frame->type != MULTIWIFI_FRAME_AUTH_RESP) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Frame is not AUTH_RESP");
        return FALSE;
    }

    if (frame->length < 18) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AUTH_RESP frame payload too short");
        return FALSE;
    }

    const guint8 *p = frame->payload;
    size_t offset = 0;

    uint32_t st_be;
    memcpy(&st_be, p + offset, 4);
    *out_status = GUINT32_FROM_BE(st_be);
    offset += 4;

    char vip_str[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, p + offset, vip_str, sizeof(vip_str));
    *out_vip = g_strdup(vip_str);
    offset += 4;

    char mask_str[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, p + offset, mask_str, sizeof(mask_str));
    *out_netmask = g_strdup(mask_str);
    offset += 4;

    uint32_t mtu_be;
    memcpy(&mtu_be, p + offset, 4);
    *out_mtu = GUINT32_FROM_BE(mtu_be);
    offset += 4;

    uint16_t msg_len_be;
    memcpy(&msg_len_be, p + offset, 2);
    uint16_t msg_len = GUINT16_FROM_BE(msg_len_be);
    offset += 2;

    if (offset + msg_len > frame->length) {
        g_free(*out_vip);
        *out_vip = NULL;
        g_free(*out_netmask);
        *out_netmask = NULL;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Malformed message in AUTH_RESP");
        return FALSE;
    }

    *out_msg = g_strndup((const gchar *)(p + offset), msg_len);
    return TRUE;
}

GByteArray *multiwifi_frame_encode_ip(uint32_t sequence,
                                      const guint8 *packet,
                                      uint32_t packet_len)
{
    return multiwifi_frame_encode(MULTIWIFI_FRAME_IP_PACKET, 0, sequence, packet, packet_len);
}

GByteArray *multiwifi_frame_encode_ping(uint32_t sequence, uint64_t timestamp_us)
{
    uint64_t ts_be = GUINT64_TO_BE(timestamp_us);
    return multiwifi_frame_encode(MULTIWIFI_FRAME_PING, 0, sequence, (const guint8 *)&ts_be, 8);
}

GByteArray *multiwifi_frame_encode_pong(uint32_t sequence,
                                        uint64_t client_ts_us,
                                        uint64_t server_ts_us)
{
    guint8 buf[16];
    uint64_t c_be = GUINT64_TO_BE(client_ts_us);
    uint64_t s_be = GUINT64_TO_BE(server_ts_us);
    memcpy(buf, &c_be, 8);
    memcpy(buf + 8, &s_be, 8);
    return multiwifi_frame_encode(MULTIWIFI_FRAME_PONG, 0, sequence, buf, 16);
}

GByteArray *multiwifi_frame_encode_disconnect(uint32_t sequence, uint32_t reason_code)
{
    uint32_t r_be = GUINT32_TO_BE(reason_code);
    return multiwifi_frame_encode(MULTIWIFI_FRAME_DISCONNECT, 0, sequence, (const guint8 *)&r_be, 4);
}

/* Stream Parser */

struct _MultiWifiFrameParser {
    GByteArray *buffer;
};

MultiWifiFrameParser *multiwifi_frame_parser_new(void)
{
    MultiWifiFrameParser *self = g_new0(MultiWifiFrameParser, 1);
    self->buffer = g_byte_array_new();
    return self;
}

void multiwifi_frame_parser_free(MultiWifiFrameParser *self)
{
    if (self == NULL) {
        return;
    }
    if (self->buffer != NULL) {
        g_byte_array_free(self->buffer, TRUE);
    }
    g_free(self);
}

void multiwifi_frame_parser_reset(MultiWifiFrameParser *self)
{
    if (self == NULL || self->buffer == NULL) {
        return;
    }
    g_byte_array_set_size(self->buffer, 0);
}

GList *multiwifi_frame_parser_feed(MultiWifiFrameParser *self,
                                   const guint8 *data,
                                   gsize len,
                                   GError **error)
{
    (void)error;
    if (self == NULL || data == NULL || len == 0) {
        return NULL;
    }

    g_byte_array_append(self->buffer, data, (guint)len);
    GList *frames = NULL;

    while (self->buffer->len >= MULTIWIFI_BONDING_HEADER_SIZE) {
        /* Check magic */
        uint32_t magic_be;
        memcpy(&magic_be, self->buffer->data, 4);
        uint32_t magic = GUINT32_FROM_BE(magic_be);

        if (magic != MULTIWIFI_BONDING_MAGIC) {
            /* Scan for next magic */
            gboolean found = FALSE;
            for (guint i = 1; i + 4 <= self->buffer->len; i++) {
                uint32_t cand_be;
                memcpy(&cand_be, self->buffer->data + i, 4);
                if (GUINT32_FROM_BE(cand_be) == MULTIWIFI_BONDING_MAGIC) {
                    g_byte_array_remove_range(self->buffer, 0, i);
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                /* Keep last 3 bytes in case magic was split across chunks */
                if (self->buffer->len > 3) {
                    g_byte_array_remove_range(self->buffer, 0, self->buffer->len - 3);
                }
                break;
            }
            continue;
        }

        /* Magic matched at index 0 */
        uint32_t payload_len_be;
        memcpy(&payload_len_be, self->buffer->data + 8, 4);
        uint32_t payload_len = GUINT32_FROM_BE(payload_len_be);

        if (payload_len > MULTIWIFI_BONDING_MAX_PAYLOAD) {
            /* Corrupted header, discard magic byte to re-sync */
            g_byte_array_remove_range(self->buffer, 0, 4);
            continue;
        }

        guint total_frame_size = MULTIWIFI_BONDING_HEADER_SIZE + payload_len;
        if (self->buffer->len < total_frame_size) {
            /* Partial frame, wait for more data */
            break;
        }

        /* Complete frame available */
        MultiWifiFrame *f = multiwifi_frame_new();
        f->magic = magic;
        f->version = self->buffer->data[4];
        f->type = self->buffer->data[5];

        uint16_t flags_be;
        memcpy(&flags_be, self->buffer->data + 6, 2);
        f->flags = GUINT16_FROM_BE(flags_be);

        f->length = payload_len;

        uint32_t seq_be;
        memcpy(&seq_be, self->buffer->data + 12, 4);
        f->sequence = GUINT32_FROM_BE(seq_be);

        if (payload_len > 0) {
            f->payload = g_memdup2(self->buffer->data + MULTIWIFI_BONDING_HEADER_SIZE, payload_len);
        }

        frames = g_list_append(frames, f);
        g_byte_array_remove_range(self->buffer, 0, total_frame_size);
    }

    return frames;
}
