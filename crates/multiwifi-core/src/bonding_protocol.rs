use bytes::{Buf, BufMut, BytesMut};
use std::net::Ipv4Addr;
use thiserror::Error;

pub const BONDING_MAGIC: u32 = 0x4d57_4631; // "MWF1"
pub const BONDING_VERSION: u8 = 1;
pub const BONDING_HEADER_SIZE: usize = 16;
pub const BONDING_MAX_PAYLOAD: usize = 65_535;
const MAX_PARSER_BUFFER_BYTES: usize = 4 * 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FrameType {
    Unknown = 0x00,
    AuthReq = 0x01,
    AuthResp = 0x02,
    IpPacket = 0x03,
    Ping = 0x04,
    Pong = 0x05,
    Disconnect = 0x06,
}

impl From<u8> for FrameType {
    fn from(raw: u8) -> Self {
        match raw {
            0x01 => Self::AuthReq,
            0x02 => Self::AuthResp,
            0x03 => Self::IpPacket,
            0x04 => Self::Ping,
            0x05 => Self::Pong,
            0x06 => Self::Disconnect,
            _ => Self::Unknown,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum AuthStatus {
    Ok = 0,
    InvalidToken = 1,
    Unauthorized = 2,
    ServerFull = 3,
    Error = 4,
}

impl AuthStatus {
    #[must_use]
    pub const fn from_u32(val: u32) -> Self {
        match val {
            0 => Self::Ok,
            1 => Self::InvalidToken,
            2 => Self::Unauthorized,
            3 => Self::ServerFull,
            _ => Self::Error,
        }
    }
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum ProtocolError {
    #[error("unexpected frame type (expected {expected:?}, got {actual:?})")]
    UnexpectedFrameType {
        expected: FrameType,
        actual: FrameType,
    },

    #[error("malformed frame payload: {0}")]
    MalformedPayload(&'static str),

    #[error("incoming frame chunk exceeds parser buffer limit")]
    BufferLimitExceeded,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub magic: u32,
    pub version: u8,
    pub frame_type: FrameType,
    pub flags: u16,
    pub sequence: u32,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthRequest {
    pub client_version: u32,
    pub profile_id: String,
    pub token: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthResponse {
    pub status: u32,
    pub assigned_vip: String,
    pub netmask: String,
    pub server_mtu: u32,
    pub message: String,
}

#[must_use]
pub fn encode_frame(frame_type: FrameType, flags: u16, sequence: u32, payload: &[u8]) -> Vec<u8> {
    let payload_len = payload.len().min(BONDING_MAX_PAYLOAD);
    let mut buf = BytesMut::with_capacity(BONDING_HEADER_SIZE + payload_len);
    buf.put_u32(BONDING_MAGIC);
    buf.put_u8(BONDING_VERSION);
    buf.put_u8(frame_type as u8);
    buf.put_u16(flags);
    buf.put_u32(payload_len as u32);
    buf.put_u32(sequence);
    if payload_len > 0 {
        buf.put_slice(&payload[..payload_len]);
    }
    buf.to_vec()
}

fn clamp_u16_str(s: &str) -> &[u8] {
    let b = s.as_bytes();
    let len = b.len().min(u16::MAX as usize);
    &b[..len]
}

#[must_use]
pub fn encode_auth_req(sequence: u32, profile_id: &str, token: &str) -> Vec<u8> {
    let pid = clamp_u16_str(profile_id);
    let tok = clamp_u16_str(token);

    let mut payload = BytesMut::with_capacity(8 + pid.len() + tok.len());
    payload.put_u32(u32::from(BONDING_VERSION));
    payload.put_u16(pid.len() as u16);
    payload.put_slice(pid);
    payload.put_u16(tok.len() as u16);
    payload.put_slice(tok);

    encode_frame(FrameType::AuthReq, 0, sequence, &payload)
}

pub fn decode_auth_req(frame: &Frame) -> Result<AuthRequest, ProtocolError> {
    if frame.frame_type != FrameType::AuthReq {
        return Err(ProtocolError::UnexpectedFrameType {
            expected: FrameType::AuthReq,
            actual: frame.frame_type,
        });
    }
    if frame.payload.len() < 8 {
        return Err(ProtocolError::MalformedPayload(
            "AUTH_REQ payload too short",
        ));
    }

    let mut slice = frame.payload.as_slice();
    let client_version = slice.get_u32();

    let pid_len = usize::from(slice.get_u16());
    if slice.len() < pid_len + 2 {
        return Err(ProtocolError::MalformedPayload(
            "malformed profile_id in AUTH_REQ",
        ));
    }
    let profile_id = String::from_utf8_lossy(&slice[..pid_len]).into_owned();
    slice.advance(pid_len);

    let tok_len = usize::from(slice.get_u16());
    if slice.len() < tok_len {
        return Err(ProtocolError::MalformedPayload(
            "malformed token in AUTH_REQ",
        ));
    }
    let token = String::from_utf8_lossy(&slice[..tok_len]).into_owned();

    Ok(AuthRequest {
        client_version,
        profile_id,
        token,
    })
}

#[must_use]
pub fn encode_auth_resp(
    sequence: u32,
    status: u32,
    assigned_vip: &str,
    netmask: &str,
    server_mtu: u32,
    message: &str,
) -> Vec<u8> {
    let vip = assigned_vip
        .parse::<Ipv4Addr>()
        .unwrap_or(Ipv4Addr::UNSPECIFIED);
    let mask = netmask.parse::<Ipv4Addr>().unwrap_or(Ipv4Addr::UNSPECIFIED);
    let msg = clamp_u16_str(message);

    let mut payload = BytesMut::with_capacity(18 + msg.len());
    payload.put_u32(status);
    payload.put_slice(&vip.octets());
    payload.put_slice(&mask.octets());
    payload.put_u32(server_mtu);
    payload.put_u16(msg.len() as u16);
    payload.put_slice(msg);

    encode_frame(FrameType::AuthResp, 0, sequence, &payload)
}

pub fn decode_auth_resp(frame: &Frame) -> Result<AuthResponse, ProtocolError> {
    if frame.frame_type != FrameType::AuthResp {
        return Err(ProtocolError::UnexpectedFrameType {
            expected: FrameType::AuthResp,
            actual: frame.frame_type,
        });
    }
    if frame.payload.len() < 18 {
        return Err(ProtocolError::MalformedPayload(
            "AUTH_RESP payload too short",
        ));
    }

    let mut slice = frame.payload.as_slice();
    let status = slice.get_u32();

    let vip = Ipv4Addr::from([slice[0], slice[1], slice[2], slice[3]]);
    slice.advance(4);

    let mask = Ipv4Addr::from([slice[0], slice[1], slice[2], slice[3]]);
    slice.advance(4);

    let server_mtu = slice.get_u32();
    let msg_len = usize::from(slice.get_u16());
    if slice.len() < msg_len {
        return Err(ProtocolError::MalformedPayload(
            "malformed message in AUTH_RESP",
        ));
    }
    let message = String::from_utf8_lossy(&slice[..msg_len]).into_owned();

    Ok(AuthResponse {
        status,
        assigned_vip: vip.to_string(),
        netmask: mask.to_string(),
        server_mtu,
        message,
    })
}

#[must_use]
pub fn encode_ip(sequence: u32, packet: &[u8]) -> Vec<u8> {
    encode_frame(FrameType::IpPacket, 0, sequence, packet)
}

#[must_use]
pub fn encode_ping(sequence: u32, timestamp_us: u64) -> Vec<u8> {
    encode_frame(FrameType::Ping, 0, sequence, &timestamp_us.to_be_bytes())
}

#[must_use]
pub fn encode_pong(sequence: u32, client_ts_us: u64, server_ts_us: u64) -> Vec<u8> {
    let mut buf = [0u8; 16];
    buf[..8].copy_from_slice(&client_ts_us.to_be_bytes());
    buf[8..].copy_from_slice(&server_ts_us.to_be_bytes());
    encode_frame(FrameType::Pong, 0, sequence, &buf)
}

#[must_use]
pub fn encode_disconnect(sequence: u32, reason_code: u32) -> Vec<u8> {
    encode_frame(
        FrameType::Disconnect,
        0,
        sequence,
        &reason_code.to_be_bytes(),
    )
}

/// Streaming frame parser with automatic garbage & bad-version resynchronization.
#[derive(Debug, Default)]
pub struct FrameParser {
    buffer: BytesMut,
}

impl FrameParser {
    #[must_use]
    pub fn new() -> Self {
        Self {
            buffer: BytesMut::with_capacity(4096),
        }
    }

    pub fn reset(&mut self) {
        self.buffer.clear();
    }

    pub fn feed(&mut self, data: &[u8]) -> Result<Vec<Frame>, ProtocolError> {
        if data.is_empty() {
            return Ok(Vec::new());
        }
        if self.buffer.len().saturating_add(data.len()) > MAX_PARSER_BUFFER_BYTES {
            return Err(ProtocolError::BufferLimitExceeded);
        }

        self.buffer.extend_from_slice(data);
        let mut frames = Vec::new();

        while self.buffer.len() >= BONDING_HEADER_SIZE {
            let magic = u32::from_be_bytes([
                self.buffer[0],
                self.buffer[1],
                self.buffer[2],
                self.buffer[3],
            ]);

            if magic != BONDING_MAGIC {
                if let Some(pos) = self.find_magic_offset(1) {
                    self.buffer.advance(pos);
                    continue;
                }
                if self.buffer.len() > 3 {
                    let keep_from = self.buffer.len() - 3;
                    self.buffer.advance(keep_from);
                }
                break;
            }

            let version = self.buffer[4];
            let payload_len = u32::from_be_bytes([
                self.buffer[8],
                self.buffer[9],
                self.buffer[10],
                self.buffer[11],
            ]) as usize;

            if version != BONDING_VERSION || payload_len > BONDING_MAX_PAYLOAD {
                self.buffer.advance(4);
                continue;
            }

            let total_size = BONDING_HEADER_SIZE + payload_len;
            if self.buffer.len() < total_size {
                break;
            }

            let frame_type = FrameType::from(self.buffer[5]);
            let flags = u16::from_be_bytes([self.buffer[6], self.buffer[7]]);
            let sequence = u32::from_be_bytes([
                self.buffer[12],
                self.buffer[13],
                self.buffer[14],
                self.buffer[15],
            ]);
            let payload = self.buffer[BONDING_HEADER_SIZE..total_size].to_vec();
            self.buffer.advance(total_size);

            frames.push(Frame {
                magic,
                version,
                frame_type,
                flags,
                sequence,
                payload,
            });
        }

        Ok(frames)
    }

    fn find_magic_offset(&self, start: usize) -> Option<usize> {
        let magic_bytes = BONDING_MAGIC.to_be_bytes();
        self.buffer
            .get(start..)?
            .windows(4)
            .position(|w| w == magic_bytes)
            .map(|idx| start + idx)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn auth_req_roundtrip() {
        let raw = encode_auth_req(42, "prof-vn-1", "my-secret-token");
        let mut parser = FrameParser::new();
        let frames = parser.feed(&raw).expect("parse auth req");
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].frame_type, FrameType::AuthReq);
        assert_eq!(frames[0].sequence, 42);

        let decoded = decode_auth_req(&frames[0]).expect("decode auth req");
        assert_eq!(decoded.client_version, 1);
        assert_eq!(decoded.profile_id, "prof-vn-1");
        assert_eq!(decoded.token, "my-secret-token");
    }

    #[test]
    fn auth_resp_roundtrip() {
        let raw = encode_auth_resp(
            42,
            AuthStatus::Ok as u32,
            "10.8.0.5",
            "255.255.255.0",
            1420,
            "Connected cleanly",
        );
        let mut parser = FrameParser::new();
        let frames = parser.feed(&raw).expect("parse auth resp");
        assert_eq!(frames.len(), 1);

        let resp = decode_auth_resp(&frames[0]).expect("decode auth resp");
        assert_eq!(resp.status, AuthStatus::Ok as u32);
        assert_eq!(resp.assigned_vip, "10.8.0.5");
        assert_eq!(resp.netmask, "255.255.255.0");
        assert_eq!(resp.server_mtu, 1420);
        assert_eq!(resp.message, "Connected cleanly");
    }

    #[test]
    fn streaming_one_byte_and_concatenated() {
        let ping = encode_ping(7, 1_234_567);
        let mut parser = FrameParser::new();
        for (idx, byte) in ping.iter().enumerate() {
            let out = parser.feed(std::slice::from_ref(byte)).expect("feed byte");
            if idx + 1 < ping.len() {
                assert!(out.is_empty());
            } else {
                assert_eq!(out.len(), 1);
                assert_eq!(out[0].frame_type, FrameType::Ping);
                assert_eq!(out[0].sequence, 7);
            }
        }

        let mut combined = encode_ping(1, 100);
        combined.extend_from_slice(&encode_disconnect(2, 0));
        let out = parser.feed(&combined).expect("concatenated");
        assert_eq!(out.len(), 2);
        assert_eq!(out[0].frame_type, FrameType::Ping);
        assert_eq!(out[1].frame_type, FrameType::Disconnect);
    }

    #[test]
    fn garbage_and_bad_version_resync() {
        let mut bad = encode_ping(10, 111);
        bad[4] = 0x99; // invalid protocol version

        let good = encode_ping(77, 5678);

        let mut stream = vec![0xAA; 64];
        stream.extend_from_slice(&bad);
        stream.extend_from_slice(&good);

        let mut parser = FrameParser::new();
        let out = parser.feed(&stream).expect("resync");
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].frame_type, FrameType::Ping);
        assert_eq!(out[0].sequence, 77);
    }

    #[test]
    fn decode_rejects_truncated_and_mismatched_frames() {
        let ping_raw = encode_ping(1, 999);
        let mut parser = FrameParser::new();
        let ping_frame = &parser.feed(&ping_raw).expect("parse")[0];
        assert!(matches!(
            decode_auth_req(ping_frame).unwrap_err(),
            ProtocolError::UnexpectedFrameType { .. }
        ));

        let truncated = Frame {
            magic: BONDING_MAGIC,
            version: BONDING_VERSION,
            frame_type: FrameType::AuthResp,
            flags: 0,
            sequence: 1,
            payload: vec![0; 4],
        };
        assert!(matches!(
            decode_auth_resp(&truncated).unwrap_err(),
            ProtocolError::MalformedPayload(_)
        ));
    }
}
