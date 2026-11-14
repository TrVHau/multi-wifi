#ifndef MULTIWIFI_BONDING_PROTOCOL_H
#define MULTIWIFI_BONDING_PROTOCOL_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

#define MULTIWIFI_BONDING_MAGIC 0x4d574631 /* "MWF1" */
#define MULTIWIFI_BONDING_VERSION 1
#define MULTIWIFI_BONDING_HEADER_SIZE 16
#define MULTIWIFI_BONDING_MAX_PAYLOAD 65535

typedef enum {
    MULTIWIFI_FRAME_UNKNOWN       = 0x00,
    MULTIWIFI_FRAME_AUTH_REQ      = 0x01,
    MULTIWIFI_FRAME_AUTH_RESP     = 0x02,
    MULTIWIFI_FRAME_IP_PACKET     = 0x03,
    MULTIWIFI_FRAME_PING          = 0x04,
    MULTIWIFI_FRAME_PONG          = 0x05,
    MULTIWIFI_FRAME_DISCONNECT    = 0x06
} MultiWifiFrameType;

typedef enum {
    MULTIWIFI_AUTH_STATUS_OK           = 0,
    MULTIWIFI_AUTH_STATUS_INVALID_TOKEN = 1,
    MULTIWIFI_AUTH_STATUS_UNAUTHORIZED  = 2,
    MULTIWIFI_AUTH_STATUS_SERVER_FULL   = 3,
    MULTIWIFI_AUTH_STATUS_ERROR         = 4
} MultiWifiAuthStatus;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t length;
    uint32_t sequence;
    guint8 *payload;
} MultiWifiFrame;

/**
 * multiwifi_frame_new:
 *
 * Allocates an empty MultiWifiFrame structure.
 *
 * Returns: (transfer full): Newly allocated MultiWifiFrame.
 */
MultiWifiFrame *multiwifi_frame_new(void);

/**
 * multiwifi_frame_free:
 * @frame: (transfer full)(nullable): Frame to free
 */
void multiwifi_frame_free(MultiWifiFrame *frame);

/**
 * multiwifi_frame_encode:
 * @type: Frame type
 * @flags: Protocol flags
 * @sequence: Sequence number
 * @payload: Payload buffer (nullable if payload_len is 0)
 * @payload_len: Length of payload in bytes
 *
 * Encodes a complete frame into wire format.
 *
 * Returns: (transfer full): GByteArray with encoded frame bytes.
 */
GByteArray *multiwifi_frame_encode(uint8_t type,
                                   uint16_t flags,
                                   uint32_t sequence,
                                   const guint8 *payload,
                                   uint32_t payload_len);

/**
 * multiwifi_frame_encode_auth_req:
 * @sequence: Sequence number
 * @profile_id: Profile identifier
 * @token: Authentication token
 *
 * Encodes an authentication request frame.
 *
 * Returns: (transfer full): GByteArray with encoded frame.
 */
GByteArray *multiwifi_frame_encode_auth_req(uint32_t sequence,
                                            const gchar *profile_id,
                                            const gchar *token);

/**
 * multiwifi_frame_decode_auth_req:
 * @frame: Received frame
 * @out_profile_id: (out)(transfer full): Extracted profile ID
 * @out_token: (out)(transfer full): Extracted token
 * @error: (out)(optional): Return location for GError
 *
 * Returns: %TRUE on success, %FALSE on malformed payload.
 */
gboolean multiwifi_frame_decode_auth_req(const MultiWifiFrame *frame,
                                         gchar **out_profile_id,
                                         gchar **out_token,
                                         GError **error);

/**
 * multiwifi_frame_encode_auth_resp:
 * @sequence: Sequence number
 * @status: Authentication status code
 * @assigned_vip: Virtual IPv4 address (e.g. "10.8.0.2")
 * @netmask: Subnet mask (e.g. "255.255.255.0")
 * @server_mtu: Server MTU
 * @message: Status message
 *
 * Returns: (transfer full): GByteArray with encoded frame.
 */
GByteArray *multiwifi_frame_encode_auth_resp(uint32_t sequence,
                                             uint32_t status,
                                             const gchar *assigned_vip,
                                             const gchar *netmask,
                                             uint32_t server_mtu,
                                             const gchar *message);

/**
 * multiwifi_frame_decode_auth_resp:
 * @frame: Received frame
 * @out_status: (out): Authentication status code
 * @out_vip: (out)(transfer full): Assigned virtual IP
 * @out_netmask: (out)(transfer full): Subnet mask
 * @out_mtu: (out): Configured MTU
 * @out_msg: (out)(transfer full): Status message
 * @error: (out)(optional): Return location for GError
 *
 * Returns: %TRUE on success, %FALSE on malformed payload.
 */
gboolean multiwifi_frame_decode_auth_resp(const MultiWifiFrame *frame,
                                          uint32_t *out_status,
                                          gchar **out_vip,
                                          gchar **out_netmask,
                                          uint32_t *out_mtu,
                                          gchar **out_msg,
                                          GError **error);

/**
 * multiwifi_frame_encode_ip:
 * @sequence: Sequence number
 * @packet: Raw IPv4 or IPv6 packet
 * @packet_len: Packet length in bytes
 *
 * Returns: (transfer full): Encoded frame bytes.
 */
GByteArray *multiwifi_frame_encode_ip(uint32_t sequence,
                                      const guint8 *packet,
                                      uint32_t packet_len);

/**
 * multiwifi_frame_encode_ping:
 * @sequence: Sequence number
 * @timestamp_us: Microsecond timestamp
 *
 * Returns: (transfer full): Encoded frame bytes.
 */
GByteArray *multiwifi_frame_encode_ping(uint32_t sequence, uint64_t timestamp_us);

/**
 * multiwifi_frame_encode_pong:
 * @sequence: Sequence number
 * @client_ts_us: Echoed client timestamp
 * @server_ts_us: Server timestamp
 *
 * Returns: (transfer full): Encoded frame bytes.
 */
GByteArray *multiwifi_frame_encode_pong(uint32_t sequence,
                                        uint64_t client_ts_us,
                                        uint64_t server_ts_us);

/**
 * multiwifi_frame_encode_disconnect:
 * @sequence: Sequence number
 * @reason_code: Disconnect reason code
 *
 * Returns: (transfer full): Encoded frame bytes.
 */
GByteArray *multiwifi_frame_encode_disconnect(uint32_t sequence, uint32_t reason_code);

/* Stream Parser */

typedef struct _MultiWifiFrameParser MultiWifiFrameParser;

/**
 * multiwifi_frame_parser_new:
 *
 * Allocates a new streaming frame parser.
 *
 * Returns: (transfer full): Newly created parser.
 */
MultiWifiFrameParser *multiwifi_frame_parser_new(void);

/**
 * multiwifi_frame_parser_free:
 * @self: (transfer full)(nullable): Parser to free
 */
void multiwifi_frame_parser_free(MultiWifiFrameParser *self);

/**
 * multiwifi_frame_parser_reset:
 * @self: Parser instance
 *
 * Clears buffered incoming bytes.
 */
void multiwifi_frame_parser_reset(MultiWifiFrameParser *self);

/**
 * multiwifi_frame_parser_feed:
 * @self: Parser instance
 * @data: Incoming byte chunk
 * @len: Length of chunk
 * @error: (out)(optional): Return location for GError
 *
 * Feeds incoming bytes into parser and extracts all completed, validated frames.
 *
 * Returns: (transfer full)(element-type MultiWifiFrame): List of MultiWifiFrame pointers.
 */
GList *multiwifi_frame_parser_feed(MultiWifiFrameParser *self,
                                   const guint8 *data,
                                   gsize len,
                                   GError **error);

G_END_DECLS

#endif /* MULTIWIFI_BONDING_PROTOCOL_H */
