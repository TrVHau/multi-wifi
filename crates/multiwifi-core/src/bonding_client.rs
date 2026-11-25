#![deny(unsafe_code)]

use crate::bonding_protocol::{
    decode_auth_resp, encode_auth_req, encode_disconnect, encode_ip, encode_ping, encode_pong,
    AuthStatus, FrameParser, FrameType, ProtocolError,
};
use crate::loop_prevention::{LoopPrevention, LoopPreventionError};
use crate::tun_device::{TunDevice, TunError, DEFAULT_TUN_MTU};
use socket2::{Domain, Protocol, Socket, Type};
use std::fmt;
use std::net::{IpAddr, SocketAddr};
use std::os::fd::OwnedFd;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant};
use thiserror::Error;
use tokio::io::unix::AsyncFd;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpSocket, TcpStream};
use tokio::sync::{watch, Mutex as AsyncMutex};
use tokio::task::JoinHandle;

const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(5);
const KEEPALIVE_INTERVAL: Duration = Duration::from_secs(3);
const DISCONNECT_SEND_TIMEOUT: Duration = Duration::from_millis(100);
const IPPROTO_MPTCP: i32 = 262;
const IO_BUF_SIZE: usize = 65_536;

#[derive(Debug, Error)]
pub enum BondingError {
    #[error("invalid bonding configuration: {0}")]
    InvalidArgument(String),

    #[error("bonding client is already running")]
    AlreadyRunning,

    #[error("failed to resolve or connect to server: {0}")]
    Connect(String),

    #[error("authentication handshake timed out or connection closed")]
    HandshakeTimeout,

    #[error("protocol error: {0}")]
    Protocol(#[from] ProtocolError),

    #[error("server rejected authentication ({status:?}): {message}")]
    AuthRejected { status: AuthStatus, message: String },

    #[error("TUN device error: {0}")]
    Tun(#[from] TunError),

    #[error("loop prevention error: {0}")]
    LoopPrevention(#[from] LoopPreventionError),

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum BondingState {
    #[default]
    Disconnected,
    Connecting,
    Authenticating,
    Connected,
    Disconnecting,
    Failed,
}

impl BondingState {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Disconnected => "Disconnected",
            Self::Connecting => "Connecting",
            Self::Authenticating => "Authenticating",
            Self::Connected => "Connected",
            Self::Disconnecting => "Disconnecting",
            Self::Failed => "Failed",
        }
    }
}

impl fmt::Display for BondingState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Clone, Default)]
pub struct BondingProfile {
    pub profile_id: String,
    pub server_host: String,
    pub server_port: u16,
    pub auth_token: String,
}

impl fmt::Debug for BondingProfile {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("BondingProfile")
            .field("profile_id", &self.profile_id)
            .field("server_host", &self.server_host)
            .field("server_port", &self.server_port)
            .field("auth_token", &"[REDACTED]")
            .finish()
    }
}

#[derive(Debug, Clone, Default)]
pub struct BondingUplink {
    pub physical_ifname: String,
    pub physical_gw: String,
}

struct ClientSnapshot {
    profile: BondingProfile,
    uplink: Option<BondingUplink>,
    tun: Option<TunDevice>,
    state: BondingState,
    is_mptcp: bool,
    virtual_ip: Option<String>,
    virtual_netmask: Option<String>,
    server_mtu: u32,
}

struct RuntimeResources {
    loop_prev: Option<LoopPrevention>,
    stop_tx: Option<watch::Sender<bool>>,
    worker: Option<JoinHandle<()>>,
}

pub struct BondingClient {
    snapshot: Arc<Mutex<ClientSnapshot>>,
    resources: Arc<AsyncMutex<RuntimeResources>>,
    lifecycle_lock: AsyncMutex<()>,
    state_tx: watch::Sender<BondingState>,
}

impl Default for BondingClient {
    fn default() -> Self {
        Self::new()
    }
}

impl BondingClient {
    #[must_use]
    pub fn new() -> Self {
        let (state_tx, _) = watch::channel(BondingState::Disconnected);
        Self {
            snapshot: Arc::new(Mutex::new(ClientSnapshot {
                profile: BondingProfile::default(),
                uplink: None,
                tun: None,
                state: BondingState::Disconnected,
                is_mptcp: false,
                virtual_ip: None,
                virtual_netmask: None,
                server_mtu: DEFAULT_TUN_MTU,
            })),
            resources: Arc::new(AsyncMutex::new(RuntimeResources {
                loop_prev: None,
                stop_tx: None,
                worker: None,
            })),
            lifecycle_lock: AsyncMutex::new(()),
            state_tx,
        }
    }

    pub fn set_profile(&self, profile: BondingProfile) {
        let mut guard = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
        guard.profile = profile;
    }

    pub fn set_uplink(&self, uplink: Option<BondingUplink>) {
        let mut guard = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
        guard.uplink = uplink;
    }

    pub fn set_tun_device(&self, tun: Option<TunDevice>) -> Result<(), BondingError> {
        let mut guard = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
        if matches!(
            guard.state,
            BondingState::Connecting
                | BondingState::Authenticating
                | BondingState::Connected
                | BondingState::Disconnecting
        ) {
            return Err(BondingError::AlreadyRunning);
        }
        guard.tun = tun;
        Ok(())
    }

    #[must_use]
    pub fn state(&self) -> BondingState {
        self.snapshot
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .state
    }

    #[must_use]
    pub fn subscribe_state(&self) -> watch::Receiver<BondingState> {
        self.state_tx.subscribe()
    }

    #[must_use]
    pub fn virtual_ip(&self) -> Option<String> {
        self.snapshot
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .virtual_ip
            .clone()
    }

    #[must_use]
    pub fn is_mptcp(&self) -> bool {
        self.snapshot
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .is_mptcp
    }

    fn set_state(&self, new_state: BondingState) {
        let mut guard = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
        guard.state = new_state;
        let _ = self.state_tx.send(new_state);
    }

    fn prepare_tun(
        &self,
        vip: &str,
        netmask: &str,
        mtu: u32,
        is_mptcp: bool,
    ) -> Result<AsyncFd<OwnedFd>, BondingError> {
        let mut snap = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
        snap.is_mptcp = is_mptcp;
        snap.virtual_ip = Some(vip.to_owned());
        snap.virtual_netmask = Some(netmask.to_owned());
        snap.server_mtu = mtu;

        if snap.tun.is_none() {
            snap.tun = Some(TunDevice::new(None, mtu));
        }
        let tun = snap.tun.as_mut().expect("just ensured Some");
        if !tun.is_open() {
            tun.open()?;
        }
        tun.configure_ip(vip, netmask)?;
        Ok(tun.clone_async_fd()?)
    }

    async fn join_stale_worker(&self) -> Result<(), BondingError> {
        let finished_worker = {
            let mut res = self.resources.lock().await;
            if res.worker.is_some() {
                if self.state() == BondingState::Connected {
                    return Err(BondingError::AlreadyRunning);
                }
                res.worker.take()
            } else {
                None
            }
        };
        if let Some(w) = finished_worker {
            let _ = w.await;
        }
        Ok(())
    }

    /// Connects to the bonding server, authenticates, configures TUN, and starts packet forwarding.
    pub async fn start(&self) -> Result<(), BondingError> {
        let _guard = self.lifecycle_lock.lock().await;
        self.join_stale_worker().await?;

        let (profile, uplink) = {
            let snap = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
            (snap.profile.clone(), snap.uplink.clone())
        };

        if profile.server_host.trim().is_empty() || profile.server_port == 0 {
            return Err(BondingError::InvalidArgument(
                "server host and port must be specified".to_owned(),
            ));
        }

        self.set_state(BondingState::Connecting);
        let target = resolve_ipv4(&profile.server_host, profile.server_port)
            .await
            .inspect_err(|_| self.set_state(BondingState::Failed))?;

        let mut loop_prev = apply_uplink_loop_prevention(target, uplink)
            .await
            .inspect_err(|_| self.set_state(BondingState::Failed))?;

        let (mut stream, is_mptcp) = match connect_mptcp_or_tcp(target).await {
            Ok(conn) => conn,
            Err(err) => {
                rollback_loop_prev(&mut loop_prev).await;
                self.set_state(BondingState::Failed);
                return Err(err);
            }
        };

        self.set_state(BondingState::Authenticating);
        let auth_resp = match perform_handshake(&mut stream, &profile).await {
            Ok(resp) => resp,
            Err(err) => {
                rollback_loop_prev(&mut loop_prev).await;
                self.set_state(BondingState::Failed);
                return Err(err);
            }
        };

        let status = AuthStatus::from_u32(auth_resp.status);
        if status != AuthStatus::Ok {
            rollback_loop_prev(&mut loop_prev).await;
            self.set_state(BondingState::Failed);
            return Err(BondingError::AuthRejected {
                status,
                message: auth_resp.message,
            });
        }

        let mtu = if auth_resp.server_mtu > 0 {
            auth_resp.server_mtu
        } else {
            DEFAULT_TUN_MTU
        };

        let async_tun =
            match self.prepare_tun(&auth_resp.assigned_vip, &auth_resp.netmask, mtu, is_mptcp) {
                Ok(fd) => fd,
                Err(err) => {
                    rollback_loop_prev(&mut loop_prev).await;
                    self.set_state(BondingState::Failed);
                    return Err(err);
                }
            };

        let (stop_tx, stop_rx) = watch::channel(false);
        let snapshot_ref = Arc::clone(&self.snapshot);
        let state_tx_ref = self.state_tx.clone();

        let worker = tokio::spawn(async move {
            let unexpected_fail = run_forwarding_loop(stream, async_tun, stop_rx).await;
            if unexpected_fail {
                let mut snap = snapshot_ref.lock().unwrap_or_else(PoisonError::into_inner);
                if snap.state != BondingState::Disconnecting {
                    snap.state = BondingState::Failed;
                    let _ = state_tx_ref.send(BondingState::Failed);
                }
            }
        });

        {
            let mut res = self.resources.lock().await;
            res.loop_prev = loop_prev;
            res.stop_tx = Some(stop_tx);
            res.worker = Some(worker);
        }

        self.set_state(BondingState::Connected);
        Ok(())
    }

    /// Gracefully stops forwarding, closes the TUN device, and rolls back loop-prevention routes.
    pub async fn stop(&self) {
        let _guard = self.lifecycle_lock.lock().await;

        let (worker, mut loop_prev) = {
            let mut res = self.resources.lock().await;
            if res.worker.is_none()
                && res.loop_prev.is_none()
                && self.state() == BondingState::Disconnected
            {
                return;
            }
            self.set_state(BondingState::Disconnecting);
            if let Some(tx) = res.stop_tx.take() {
                let _ = tx.send(true);
            }
            (res.worker.take(), res.loop_prev.take())
        };

        if let Some(handle) = worker {
            let _ = handle.await;
        }

        {
            let mut snap = self.snapshot.lock().unwrap_or_else(PoisonError::into_inner);
            if let Some(tun) = snap.tun.as_mut() {
                tun.close();
            }
            snap.is_mptcp = false;
        }

        rollback_loop_prev(&mut loop_prev).await;
        self.set_state(BondingState::Disconnected);
    }
}

impl Drop for BondingClient {
    fn drop(&mut self) {
        if let Ok(mut res) = self.resources.try_lock() {
            if let Some(tx) = res.stop_tx.take() {
                let _ = tx.send(true);
            }
            if let Some(handle) = res.worker.take() {
                handle.abort();
            }
        }
    }
}

/// Validates inbound IPv4 packets before writing them into the host TUN device (anti-spoofing).
#[must_use]
pub fn is_valid_inbound_ip_packet(data: &[u8]) -> bool {
    if data.len() < 20 {
        return false;
    }

    let version = data[0] >> 4;
    let ihl = usize::from(data[0] & 0x0f);
    if version != 4 || ihl < 5 {
        return false;
    }

    let total_len = usize::from(u16::from_be_bytes([data[2], data[3]]));
    if total_len < ihl * 4 || total_len > data.len() {
        return false;
    }

    let saddr = u32::from_be_bytes([data[12], data[13], data[14], data[15]]);
    let is_loopback = (saddr & 0xff00_0000) == 0x7f00_0000;
    let is_multicast = (saddr & 0xf000_0000) == 0xe000_0000;
    let is_bcast_or_unspec = saddr == 0xffff_ffff || saddr == 0x0000_0000;

    !(is_loopback || is_multicast || is_bcast_or_unspec)
}

async fn apply_uplink_loop_prevention(
    target: SocketAddr,
    uplink: Option<BondingUplink>,
) -> Result<Option<LoopPrevention>, BondingError> {
    let Some(up) = uplink else {
        return Ok(None);
    };
    let IpAddr::V4(v4) = target.ip() else {
        return Ok(None);
    };
    if v4.is_loopback() || up.physical_ifname.is_empty() || up.physical_gw.is_empty() {
        return Ok(None);
    }

    let mut lp = LoopPrevention::new(&v4.to_string(), &up.physical_gw, &up.physical_ifname, 10);
    lp.apply().await?;
    Ok(Some(lp))
}

async fn resolve_ipv4(host: &str, port: u16) -> Result<SocketAddr, BondingError> {
    let addrs = tokio::net::lookup_host((host, port))
        .await
        .map_err(|e| BondingError::Connect(format!("DNS lookup for '{host}' failed: {e}")))?;
    addrs
        .into_iter()
        .find(SocketAddr::is_ipv4)
        .ok_or_else(|| BondingError::Connect(format!("no IPv4 address resolved for '{host}'")))
}

async fn connect_mptcp_or_tcp(target: SocketAddr) -> Result<(TcpStream, bool), BondingError> {
    let (sock, is_mptcp) = match Socket::new(
        Domain::IPV4,
        Type::STREAM,
        Some(Protocol::from(IPPROTO_MPTCP)),
    ) {
        Ok(s) => (s, true),
        Err(_) => (
            Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))
                .map_err(|e| BondingError::Connect(format!("socket creation failed: {e}")))?,
            false,
        ),
    };

    sock.set_nonblocking(true)?;
    let std_stream: std::net::TcpStream = sock.into();
    let tokio_sock = TcpSocket::from_std_stream(std_stream);
    let stream = tokio::time::timeout(HANDSHAKE_TIMEOUT, tokio_sock.connect(target))
        .await
        .map_err(|_| BondingError::Connect(format!("connect timeout to {target}")))?
        .map_err(|e| BondingError::Connect(format!("failed to connect to {target}: {e}")))?;
    Ok((stream, is_mptcp))
}

async fn perform_handshake(
    stream: &mut TcpStream,
    profile: &BondingProfile,
) -> Result<crate::bonding_protocol::AuthResponse, BondingError> {
    tokio::time::timeout(HANDSHAKE_TIMEOUT, async {
        let req = encode_auth_req(1, &profile.profile_id, &profile.auth_token);
        stream.write_all(&req).await?;

        let mut parser = FrameParser::new();
        let mut buf = [0u8; 2048];
        loop {
            let n = stream.read(&mut buf).await?;
            if n == 0 {
                return Err(BondingError::HandshakeTimeout);
            }
            let frames = parser.feed(&buf[..n])?;
            if let Some(frame) = frames.into_iter().next() {
                if frame.frame_type != FrameType::AuthResp {
                    return Err(BondingError::Connect(format!(
                        "unexpected handshake frame type {:?}",
                        frame.frame_type
                    )));
                }
                return Ok(decode_auth_resp(&frame)?);
            }
        }
    })
    .await
    .map_err(|_| BondingError::HandshakeTimeout)?
}

async fn run_forwarding_loop(
    mut stream: TcpStream,
    tun_async: AsyncFd<OwnedFd>,
    mut stop_rx: watch::Receiver<bool>,
) -> bool {
    let mut parser = FrameParser::new();
    let mut sock_buf = vec![0u8; IO_BUF_SIZE];
    let mut tun_buf = vec![0u8; IO_BUF_SIZE];
    let start = Instant::now();
    let mut seq: u32 = 10;

    let mut ping_ticker = tokio::time::interval(KEEPALIVE_INTERVAL);
    ping_ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    ping_ticker.tick().await;

    loop {
        tokio::select! {
            changed = stop_rx.changed() => {
                if changed.is_err() || *stop_rx.borrow() {
                    let disc = encode_disconnect(999, 0);
                    let _ = tokio::time::timeout(DISCONNECT_SEND_TIMEOUT, stream.write_all(&disc)).await;
                    let _ = stream.shutdown().await;
                    return false;
                }
            }
            ready = tun_async.readable() => {
                let Ok(mut guard) = ready else {
                    return true;
                };
                match guard.try_io(|inner| {
                    nix::unistd::read(inner.get_ref(), &mut tun_buf)
                        .map_err(|e| std::io::Error::from_raw_os_error(e as i32))
                }) {
                    Ok(Ok(n)) if n > 0 => {
                        let pkt = encode_ip(seq, &tun_buf[..n]);
                        seq = seq.wrapping_add(1);
                        if stream.write_all(&pkt).await.is_err() {
                            return true;
                        }
                    }
                    Err(_would_block) => {}
                    Ok(Ok(_) | Err(_)) => return true,
                }
            }
            read_res = stream.read(&mut sock_buf) => {
                match read_res {
                    Ok(0) | Err(_) => return true,
                    Ok(n) => {
                        let Ok(frames) = parser.feed(&sock_buf[..n]) else {
                            return true;
                        };
                        for frame in frames {
                            match frame.frame_type {
                                FrameType::IpPacket
                                    if !frame.payload.is_empty()
                                        && is_valid_inbound_ip_packet(&frame.payload)
                                        && !write_tun_packet(&tun_async, &frame.payload).await =>
                                {
                                    return true;
                                }
                                FrameType::Ping => {
                                    let client_ts = if frame.payload.len() >= 8 {
                                        u64::from_be_bytes([
                                            frame.payload[0], frame.payload[1], frame.payload[2], frame.payload[3],
                                            frame.payload[4], frame.payload[5], frame.payload[6], frame.payload[7],
                                        ])
                                    } else {
                                        0
                                    };
                                    let now_us = u64::try_from(start.elapsed().as_micros()).unwrap_or(u64::MAX);
                                    let pong = encode_pong(seq, client_ts, now_us);
                                    seq = seq.wrapping_add(1);
                                    if stream.write_all(&pong).await.is_err() {
                                        return true;
                                    }
                                }
                                FrameType::Disconnect => return true,
                                _ => {}
                            }
                        }
                    }
                }
            }
            _ = ping_ticker.tick() => {
                let now_us = u64::try_from(start.elapsed().as_micros()).unwrap_or(u64::MAX);
                let ping = encode_ping(seq, now_us);
                seq = seq.wrapping_add(1);
                if stream.write_all(&ping).await.is_err() {
                    return true;
                }
            }
        }
    }
}

async fn write_tun_packet(tun_async: &AsyncFd<OwnedFd>, payload: &[u8]) -> bool {
    loop {
        let Ok(mut guard) = tun_async.writable().await else {
            return false;
        };
        match guard.try_io(|inner| {
            nix::unistd::write(inner.get_ref(), payload)
                .map_err(|e| std::io::Error::from_raw_os_error(e as i32))
        }) {
            Ok(Ok(_)) => return true,
            Err(_would_block) => {}
            Ok(Err(_)) => return false,
        }
    }
}

async fn rollback_loop_prev(lp: &mut Option<LoopPrevention>) {
    if let Some(ref mut prev) = lp {
        let _ = prev.rollback().await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bonding_protocol::{decode_auth_req, encode_auth_resp};
    use nix::sys::socket::{socketpair, AddressFamily, SockFlag, SockType};
    use nix::unistd::{read as fd_read, write as fd_write};
    use tokio::net::TcpListener;

    #[test]
    fn profile_debug_redacts_secret_token() {
        let profile = BondingProfile {
            profile_id: "prof-1".to_owned(),
            server_host: "127.0.0.1".to_owned(),
            server_port: 9000,
            auth_token: "super-secret-token-value".to_owned(),
        };
        let rendered = format!("{profile:?}");
        assert!(!rendered.contains("super-secret-token-value"));
        assert!(rendered.contains("[REDACTED]"));
    }

    #[test]
    fn anti_spoofing_validates_ipv4_packets() {
        let valid_pkt: [u8; 20] = [
            0x45, 0x00, 0x00, 0x14, 0x12, 0x34, 0x40, 0x00, 0x40, 0x06, 0x00, 0x00, 0x0a, 0x08,
            0x00, 0x01, 0x0a, 0x08, 0x00, 0x2a,
        ];
        assert!(is_valid_inbound_ip_packet(&valid_pkt));
        assert!(!is_valid_inbound_ip_packet(&valid_pkt[..10]));

        // Loopback source 127.0.0.1
        let mut loopback = valid_pkt;
        loopback[12..16].copy_from_slice(&[127, 0, 0, 1]);
        assert!(!is_valid_inbound_ip_packet(&loopback));

        // Multicast source 224.0.0.1
        let mut mcast = valid_pkt;
        mcast[12..16].copy_from_slice(&[224, 0, 0, 1]);
        assert!(!is_valid_inbound_ip_packet(&mcast));

        // Broadcast / Unspecified
        let mut bcast = valid_pkt;
        bcast[12..16].copy_from_slice(&[255, 255, 255, 255]);
        assert!(!is_valid_inbound_ip_packet(&bcast));
        let mut unspec = valid_pkt;
        unspec[12..16].copy_from_slice(&[0, 0, 0, 0]);
        assert!(!is_valid_inbound_ip_packet(&unspec));
    }

    #[tokio::test]
    async fn mock_server_auth_relay_anti_spoofing_and_stop() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().expect("addr").port();

        let server_task = tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.expect("accept");
            let mut parser = FrameParser::new();
            let mut buf = [0u8; 2048];
            let mut got_disconnect = false;

            loop {
                let n = sock.read(&mut buf).await.expect("read");
                if n == 0 {
                    break;
                }
                for frame in parser.feed(&buf[..n]).expect("feed") {
                    match frame.frame_type {
                        FrameType::AuthReq => {
                            let req = decode_auth_req(&frame).expect("auth req");
                            assert_eq!(req.profile_id, "test-prof");
                            assert_eq!(req.token, "auth-token-123");
                            let resp = encode_auth_resp(
                                1,
                                AuthStatus::Ok as u32,
                                "10.8.0.42",
                                "255.255.255.0",
                                1400,
                                "Mock Authorized",
                            );
                            sock.write_all(&resp).await.expect("send auth resp");
                        }
                        FrameType::IpPacket => {
                            let echo = encode_ip(frame.sequence, &frame.payload);
                            sock.write_all(&echo).await.expect("echo ip");
                        }
                        FrameType::Ping => {
                            let pong = encode_pong(frame.sequence, 100, 200);
                            sock.write_all(&pong).await.expect("send pong");
                        }
                        FrameType::Disconnect => {
                            got_disconnect = true;
                            return got_disconnect;
                        }
                        _ => {}
                    }
                }
            }
            got_disconnect
        });

        let (sv0, sv1) = socketpair(
            AddressFamily::Unix,
            SockType::Stream,
            None,
            SockFlag::SOCK_CLOEXEC,
        )
        .expect("socketpair");
        let peer_async = crate::tun_device::register_owned_async_fd(sv1).expect("peer async");

        let client = BondingClient::new();
        client
            .set_tun_device(Some(
                TunDevice::from_mock_fd(Some("mwtun8"), 1400, sv0).expect("tun"),
            ))
            .expect("set tun");
        client.set_profile(BondingProfile {
            profile_id: "test-prof".to_owned(),
            server_host: "127.0.0.1".to_owned(),
            server_port: port,
            auth_token: "auth-token-123".to_owned(),
        });

        client.start().await.expect("start client");
        assert_eq!(client.state(), BondingState::Connected);
        assert_eq!(client.virtual_ip().as_deref(), Some("10.8.0.42"));

        // Valid IPv4 packet: 10.8.0.1 -> 10.8.0.42 echoes back cleanly.
        let valid_pkt: [u8; 20] = [
            0x45, 0x00, 0x00, 0x14, 0x12, 0x34, 0x40, 0x00, 0x40, 0x06, 0x00, 0x00, 0x0a, 0x08,
            0x00, 0x01, 0x0a, 0x08, 0x00, 0x2a,
        ];
        fd_write(&peer_async, &valid_pkt).expect("send to tun");

        let mut echo_buf = [0u8; 64];
        let mut ready = tokio::time::timeout(Duration::from_secs(2), peer_async.readable())
            .await
            .expect("echo readable timeout")
            .expect("readable");
        let n_echo = ready
            .try_io(|inner| {
                fd_read(inner.get_ref(), &mut echo_buf)
                    .map_err(|e| std::io::Error::from_raw_os_error(e as i32))
            })
            .expect("try_io")
            .expect("read echo");
        ready.clear_ready();
        assert_eq!(&echo_buf[..n_echo], &valid_pkt);

        // Spoofed 127.0.0.1 packet must be dropped by client before hitting mock TUN.
        let mut spoofed = valid_pkt;
        spoofed[12..16].copy_from_slice(&[127, 0, 0, 1]);
        fd_write(&peer_async, &spoofed).expect("send spoofed");
        let spoof_wait =
            tokio::time::timeout(Duration::from_millis(200), peer_async.readable()).await;
        assert!(spoof_wait.is_err(), "spoofed packet should be dropped");

        client.stop().await;
        assert_eq!(client.state(), BondingState::Disconnected);
        assert!(server_task.await.expect("server join"));
    }

    #[tokio::test]
    async fn auth_rejection_and_invalid_config_fail_cleanly() {
        let client = BondingClient::new();
        assert!(matches!(
            client.start().await.unwrap_err(),
            BondingError::InvalidArgument(_)
        ));

        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().expect("addr").port();
        tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.expect("accept");
            let mut buf = [0u8; 512];
            let _ = sock.read(&mut buf).await;
            let resp = encode_auth_resp(
                1,
                AuthStatus::Unauthorized as u32,
                "",
                "",
                0,
                "Invalid token",
            );
            let _ = sock.write_all(&resp).await;
        });

        client.set_profile(BondingProfile {
            profile_id: "p".to_owned(),
            server_host: "127.0.0.1".to_owned(),
            server_port: port,
            auth_token: "wrong".to_owned(),
        });
        let err = client.start().await.unwrap_err();
        assert!(matches!(
            err,
            BondingError::AuthRejected {
                status: AuthStatus::Unauthorized,
                ..
            }
        ));
        assert_eq!(client.state(), BondingState::Failed);
    }
}
