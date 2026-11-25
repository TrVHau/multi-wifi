#![deny(unsafe_code)]

use crate::config::LoadBalanceConfig;
use crate::netlink_route::parse_ipv4_clean;
use futures_util::future::{join_all, BoxFuture};
use std::fmt;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant};
use tokio::net::TcpSocket;
use tokio::sync::watch;
use tokio::task::JoinHandle;

pub const DEFAULT_HEALTH_CHECK_INTERVAL_MS: u32 = 2000;
pub const DEFAULT_FAILURE_THRESHOLD: u32 = 3;
pub const DEFAULT_RECOVERY_THRESHOLD: u32 = 3;
pub const DEFAULT_COOLDOWN_MS: u32 = 5000;
pub const ROLLING_WINDOW_SIZE: usize = 10;

const PROBE_PORTS: [u16; 3] = [53, 80, 443];
const PROBE_TIMEOUT_MS: u64 = 250;
const DEGRADED_RTT_MS: f64 = 350.0;
const DEGRADED_LOSS_PERCENT: u32 = 30;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PathState {
    #[default]
    Unknown,
    Offline,
    Probing,
    Online,
    Degraded,
    LocalOnly,
}

impl PathState {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Online => "Online",
            Self::Degraded => "Degraded",
            Self::LocalOnly => "Local Only",
            Self::Probing => "Probing",
            Self::Offline => "Offline",
            Self::Unknown => "Unknown",
        }
    }

    #[must_use]
    pub fn from_str_lossy(raw: &str) -> Self {
        if raw.eq_ignore_ascii_case("Online") {
            Self::Online
        } else if raw.eq_ignore_ascii_case("Degraded") {
            Self::Degraded
        } else if raw.eq_ignore_ascii_case("Local Only") {
            Self::LocalOnly
        } else if raw.eq_ignore_ascii_case("Probing") {
            Self::Probing
        } else if raw.eq_ignore_ascii_case("Offline") {
            Self::Offline
        } else {
            Self::Unknown
        }
    }
}

impl fmt::Display for PathState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct PathInfo {
    pub ifname: String,
    pub gateway: Option<String>,
    pub source_ip: Option<String>,
    pub state: PathState,
    pub status_reason: String,
    pub rtt_ms: f64,
    pub min_rtt_ms: f64,
    pub max_rtt_ms: f64,
    pub jitter_ms: f64,
    pub packet_loss_percent: u32,
    pub consecutive_successes: u32,
    pub consecutive_failures: u32,
    pub last_probe_at: Option<Instant>,
    pub last_state_change_at: Option<Instant>,
    pub is_eligible: bool,
}

impl PathInfo {
    #[must_use]
    pub fn new(ifname: &str) -> Self {
        Self {
            ifname: ifname.to_owned(),
            gateway: None,
            source_ip: None,
            state: PathState::Unknown,
            status_reason: String::new(),
            rtt_ms: 0.0,
            min_rtt_ms: 0.0,
            max_rtt_ms: 0.0,
            jitter_ms: 0.0,
            packet_loss_percent: 0,
            consecutive_successes: 0,
            consecutive_failures: 0,
            last_probe_at: None,
            last_state_change_at: None,
            is_eligible: false,
        }
    }
}

/// Device inventory update fed into [`PathMonitor`].
#[derive(Debug, Clone, Copy)]
pub struct DevicePathInput<'a> {
    pub ifname: &'a str,
    pub connected: bool,
    pub status_reason: &'a str,
    pub ipv4_address: Option<&'a str>,
    pub ipv4_gateway: Option<&'a str>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProbeTarget {
    pub ifname: String,
    pub source_ip: Option<String>,
    pub gateway: String,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ProbeOutcome {
    pub success: bool,
    pub rtt_ms: f64,
}

pub type ProbeFn = Arc<dyn Fn(ProbeTarget) -> BoxFuture<'static, ProbeOutcome> + Send + Sync>;

#[derive(Debug, Clone)]
struct MonitorConfig {
    interval_ms: u32,
    failure_threshold: u32,
    recovery_threshold: u32,
    cooldown_ms: u32,
}

impl Default for MonitorConfig {
    fn default() -> Self {
        Self {
            interval_ms: DEFAULT_HEALTH_CHECK_INTERVAL_MS,
            failure_threshold: DEFAULT_FAILURE_THRESHOLD,
            recovery_threshold: DEFAULT_RECOVERY_THRESHOLD,
            cooldown_ms: DEFAULT_COOLDOWN_MS,
        }
    }
}

#[derive(Debug, Clone)]
struct PathEntry {
    info: PathInfo,
    history: [bool; ROLLING_WINDOW_SIZE],
    history_len: usize,
    history_idx: usize,
    has_probe_sample: bool,
}

impl PathEntry {
    fn new(ifname: &str) -> Self {
        Self {
            info: PathInfo::new(ifname),
            history: [false; ROLLING_WINDOW_SIZE],
            history_len: 0,
            history_idx: 0,
            has_probe_sample: false,
        }
    }

    fn apply_probe_result(&mut self, cfg: &MonitorConfig, outcome: ProbeOutcome, now: Instant) {
        self.history[self.history_idx] = outcome.success;
        self.history_idx = (self.history_idx + 1) % ROLLING_WINDOW_SIZE;
        if self.history_len < ROLLING_WINDOW_SIZE {
            self.history_len += 1;
        }

        let ok_count = self.history[..self.history_len]
            .iter()
            .filter(|&&ok| ok)
            .count();
        self.info.packet_loss_percent =
            u32::try_from(((self.history_len - ok_count) * 100) / self.history_len.max(1))
                .unwrap_or(100);
        self.info.last_probe_at = Some(now);

        if outcome.success {
            let measured = outcome.rtt_ms.max(0.0);
            if self.has_probe_sample {
                self.info.jitter_ms = (measured - self.info.rtt_ms).abs();
                self.info.rtt_ms = 0.7 * self.info.rtt_ms + 0.3 * measured;
                if measured < self.info.min_rtt_ms {
                    self.info.min_rtt_ms = measured;
                }
                if measured > self.info.max_rtt_ms {
                    self.info.max_rtt_ms = measured;
                }
            } else {
                self.info.rtt_ms = measured;
                self.info.min_rtt_ms = measured;
                self.info.max_rtt_ms = measured;
                self.info.jitter_ms = 0.0;
                self.has_probe_sample = true;
            }

            self.info.consecutive_successes = self.info.consecutive_successes.saturating_add(1);
            self.info.consecutive_failures = 0;

            if self.info.state == PathState::Online {
                if self.info.rtt_ms > DEGRADED_RTT_MS
                    || self.info.packet_loss_percent >= DEGRADED_LOSS_PERCENT
                {
                    self.info.state = PathState::Degraded;
                    self.info.is_eligible = false;
                    self.info.last_state_change_at = Some(now);
                    "Elevated latency or packet loss".clone_into(&mut self.info.status_reason);
                } else {
                    "Healthy uplink".clone_into(&mut self.info.status_reason);
                }
            } else {
                let cooldown_ok = match self.info.state {
                    PathState::Probing | PathState::Unknown => true,
                    _ => self.info.last_state_change_at.is_none_or(|t| {
                        now.saturating_duration_since(t)
                            >= Duration::from_millis(u64::from(cfg.cooldown_ms))
                    }),
                };

                let quality_ok = self.info.rtt_ms <= DEGRADED_RTT_MS
                    && self.info.packet_loss_percent < DEGRADED_LOSS_PERCENT;

                if self.info.consecutive_successes >= cfg.recovery_threshold
                    && cooldown_ok
                    && (self.info.state != PathState::Degraded || quality_ok)
                {
                    self.info.state = PathState::Online;
                    self.info.is_eligible = true;
                    self.info.last_state_change_at = Some(now);
                    "Recovered and passing health probes".clone_into(&mut self.info.status_reason);
                }
            }
        } else {
            self.info.consecutive_failures = self.info.consecutive_failures.saturating_add(1);
            self.info.consecutive_successes = 0;

            if matches!(self.info.state, PathState::Online | PathState::Degraded)
                && self.info.consecutive_failures >= cfg.failure_threshold
            {
                self.info.state = PathState::LocalOnly;
                self.info.is_eligible = false;
                self.info.last_state_change_at = Some(now);
                "Gateway/path probes failing".clone_into(&mut self.info.status_reason);
            }
        }
    }
}

struct MonitorState {
    entries: Vec<PathEntry>,
    config: MonitorConfig,
    custom_probe: Option<ProbeFn>,
    probing_in_flight: bool,
    worker: Option<JoinHandle<()>>,
    stop_tx: Option<watch::Sender<bool>>,
}

#[derive(Clone)]
pub struct PathMonitor {
    state: Arc<Mutex<MonitorState>>,
    updates_tx: Arc<watch::Sender<Vec<PathInfo>>>,
}

impl Default for PathMonitor {
    fn default() -> Self {
        Self::new()
    }
}

impl PathMonitor {
    #[must_use]
    pub fn new() -> Self {
        let (updates_tx, _) = watch::channel(Vec::new());
        Self {
            state: Arc::new(Mutex::new(MonitorState {
                entries: Vec::new(),
                config: MonitorConfig::default(),
                custom_probe: None,
                probing_in_flight: false,
                worker: None,
                stop_tx: None,
            })),
            updates_tx: Arc::new(updates_tx),
        }
    }

    pub fn set_config(&self, config: &LoadBalanceConfig) {
        let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        guard.config = MonitorConfig {
            interval_ms: config.health_check_interval_ms.max(100),
            failure_threshold: config.failure_threshold.max(1),
            recovery_threshold: config.recovery_threshold.max(1),
            cooldown_ms: config.cooldown_ms,
        };
    }

    pub fn set_probe_fn(&self, probe: Option<ProbeFn>) {
        let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        guard.custom_probe = probe;
    }

    pub fn set_sync_probe_fn<F>(&self, probe: F)
    where
        F: Fn(&ProbeTarget) -> ProbeOutcome + Send + Sync + 'static,
    {
        let arc_fn: ProbeFn = Arc::new(move |target| {
            let outcome = probe(&target);
            Box::pin(std::future::ready(outcome))
        });
        self.set_probe_fn(Some(arc_fn));
    }

    #[must_use]
    pub fn subscribe(&self) -> watch::Receiver<Vec<PathInfo>> {
        self.updates_tx.subscribe()
    }

    pub fn update_device(&self, input: DevicePathInput<'_>) {
        if input.ifname.is_empty() {
            return;
        }
        let snapshot = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            let entry = if let Some(existing) = guard
                .entries
                .iter_mut()
                .find(|e| e.info.ifname == input.ifname)
            {
                existing
            } else {
                guard.entries.push(PathEntry::new(input.ifname));
                guard.entries.last_mut().expect("just pushed")
            };

            entry.info.gateway = input.ipv4_gateway.map(str::to_owned);
            entry.info.source_ip = input.ipv4_address.map(str::to_owned);

            if !input.connected {
                entry.info.state = PathState::Offline;
                entry.info.is_eligible = false;
                entry.info.consecutive_successes = 0;
                entry.info.consecutive_failures = 0;
                input
                    .status_reason
                    .clone_into(&mut entry.info.status_reason);
            } else if matches!(entry.info.state, PathState::Offline | PathState::Unknown) {
                entry.info.state = PathState::Probing;
                "Connected locally; initiating health probes"
                    .clone_into(&mut entry.info.status_reason);
            }

            Self::collect_infos(&guard.entries)
        };

        let _ = self.updates_tx.send(snapshot);
    }

    pub fn remove_device(&self, ifname: &str) {
        let snapshot = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            let before = guard.entries.len();
            guard.entries.retain(|e| e.info.ifname != ifname);
            if guard.entries.len() == before {
                return;
            }
            Self::collect_infos(&guard.entries)
        };
        let _ = self.updates_tx.send(snapshot);
    }

    pub fn update_path_info(&self, info: &PathInfo) {
        if info.ifname.is_empty() {
            return;
        }
        let snapshot = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            if let Some(existing) = guard
                .entries
                .iter_mut()
                .find(|e| e.info.ifname == info.ifname)
            {
                existing.info = info.clone();
            } else {
                let mut entry = PathEntry::new(&info.ifname);
                entry.info = info.clone();
                guard.entries.push(entry);
            }
            Self::collect_infos(&guard.entries)
        };
        let _ = self.updates_tx.send(snapshot);
    }

    #[must_use]
    pub fn paths(&self) -> Vec<PathInfo> {
        let guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        Self::collect_infos(&guard.entries)
    }

    #[must_use]
    pub fn find_path(&self, ifname: &str) -> Option<PathInfo> {
        let guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        guard
            .entries
            .iter()
            .find(|e| e.info.ifname == ifname)
            .map(|e| e.info.clone())
    }

    /// Executes one non-blocking round of health probes across all active paths.
    pub async fn probe_all_now(&self) {
        let (targets, probe_fn) = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            if guard.probing_in_flight {
                return;
            }
            let targets: Vec<ProbeTarget> = guard
                .entries
                .iter()
                .filter_map(|e| {
                    if e.info.state == PathState::Offline {
                        return None;
                    }
                    let gateway = e.info.gateway.as_ref()?.clone();
                    let _ = e.info.source_ip.as_ref()?;
                    Some(ProbeTarget {
                        ifname: e.info.ifname.clone(),
                        source_ip: e.info.source_ip.clone(),
                        gateway,
                    })
                })
                .collect();
            if targets.is_empty() {
                return;
            }
            guard.probing_in_flight = true;
            (targets, guard.custom_probe.clone())
        };

        let probe_futures = targets.into_iter().map(|target| {
            let custom = probe_fn.clone();
            async move {
                let outcome = match custom {
                    Some(f) => f(target.clone()).await,
                    None => default_socket_probe(&target).await,
                };
                (target, outcome)
            }
        });
        let results = join_all(probe_futures).await;
        let now = Instant::now();

        let snapshot = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            let cfg = guard.config.clone();
            for (target, outcome) in results {
                if let Some(entry) = guard
                    .entries
                    .iter_mut()
                    .find(|e| e.info.ifname == target.ifname)
                {
                    if entry.info.state != PathState::Offline
                        && entry.info.source_ip == target.source_ip
                        && entry.info.gateway.as_deref() == Some(target.gateway.as_str())
                    {
                        entry.apply_probe_result(&cfg, outcome, now);
                    }
                }
            }
            guard.probing_in_flight = false;
            Self::collect_infos(&guard.entries)
        };

        let _ = self.updates_tx.send(snapshot);
    }

    /// Starts background periodic probing on the Tokio runtime.
    pub fn start(&self) {
        let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        if guard.worker.is_some() {
            return;
        }
        let interval_ms = u64::from(guard.config.interval_ms.max(100));
        let (stop_tx, mut stop_rx) = watch::channel(false);
        let monitor = self.clone();

        let handle = tokio::spawn(async move {
            monitor.probe_all_now().await;
            let mut ticker = tokio::time::interval(Duration::from_millis(interval_ms));
            ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            ticker.tick().await;

            loop {
                tokio::select! {
                    changed = stop_rx.changed() => {
                        if changed.is_err() || *stop_rx.borrow() {
                            break;
                        }
                    }
                    _ = ticker.tick() => {
                        monitor.probe_all_now().await;
                    }
                }
            }
        });

        guard.stop_tx = Some(stop_tx);
        guard.worker = Some(handle);
    }

    /// Stops periodic probing and awaits clean worker shutdown.
    pub async fn stop(&self) {
        let handle = {
            let mut guard = self.state.lock().unwrap_or_else(PoisonError::into_inner);
            if let Some(tx) = guard.stop_tx.take() {
                let _ = tx.send(true);
            }
            guard.worker.take()
        };

        if let Some(h) = handle {
            let _ = h.await;
        }
    }

    fn collect_infos(entries: &[PathEntry]) -> Vec<PathInfo> {
        entries.iter().map(|e| e.info.clone()).collect()
    }
}

async fn default_socket_probe(target: &ProbeTarget) -> ProbeOutcome {
    let Ok(dest_ip) = parse_ipv4_clean(&target.gateway) else {
        return ProbeOutcome {
            success: false,
            rtt_ms: 0.0,
        };
    };

    let t0 = Instant::now();
    for port in PROBE_PORTS {
        if try_single_port_probe(
            &target.ifname,
            target.source_ip.as_deref(),
            dest_ip,
            port,
            PROBE_TIMEOUT_MS,
        )
        .await
        {
            return ProbeOutcome {
                success: true,
                rtt_ms: t0.elapsed().as_secs_f64() * 1000.0,
            };
        }
    }

    ProbeOutcome {
        success: false,
        rtt_ms: 0.0,
    }
}

async fn try_single_port_probe(
    ifname: &str,
    source_ip: Option<&str>,
    dest_ip: Ipv4Addr,
    port: u16,
    timeout_ms: u64,
) -> bool {
    let Ok(sock) = TcpSocket::new_v4() else {
        return false;
    };

    if !ifname.is_empty() {
        let _ = sock.bind_device(Some(ifname.as_bytes()));
    }

    if let Some(src_raw) = source_ip.filter(|s| !s.is_empty()) {
        let Ok(src_ip) = parse_ipv4_clean(src_raw) else {
            return false;
        };
        if sock
            .bind(SocketAddr::V4(SocketAddrV4::new(src_ip, 0)))
            .is_err()
        {
            return false;
        }
    }

    let remote = SocketAddr::V4(SocketAddrV4::new(dest_ip, port));
    match tokio::time::timeout(
        Duration::from_millis(timeout_ms.max(1)),
        sock.connect(remote),
    )
    .await
    {
        Ok(Ok(_stream)) => true,
        // Even ECONNREFUSED means the gateway/host replied back cleanly.
        Ok(Err(err)) => err.raw_os_error() == Some(libc::ECONNREFUSED),
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
    use tokio::net::TcpListener;

    #[test]
    fn path_state_string_roundtrip() {
        assert_eq!(PathState::Online.as_str(), "Online");
        assert_eq!(PathState::Offline.as_str(), "Offline");
        assert_eq!(PathState::from_str_lossy("Online"), PathState::Online);
        assert_eq!(PathState::from_str_lossy("degraded"), PathState::Degraded);
        assert_eq!(
            PathState::from_str_lossy("Local Only"),
            PathState::LocalOnly
        );
        assert_eq!(PathState::from_str_lossy("Probing"), PathState::Probing);
        assert_eq!(PathState::from_str_lossy("Offline"), PathState::Offline);
        assert_eq!(PathState::from_str_lossy("garbage"), PathState::Unknown);
    }

    #[tokio::test]
    async fn hysteresis_recovery_and_failure() {
        let mon = PathMonitor::new();
        let next_success = Arc::new(AtomicBool::new(true));
        let calls = Arc::new(AtomicU32::new(0));

        let next_success_c = Arc::clone(&next_success);
        let calls_c = Arc::clone(&calls);
        mon.set_sync_probe_fn(move |_| {
            calls_c.fetch_add(1, Ordering::Relaxed);
            ProbeOutcome {
                success: next_success_c.load(Ordering::Relaxed),
                rtt_ms: 15.0,
            }
        });

        let cfg = LoadBalanceConfig {
            health_check_interval_ms: 1000,
            failure_threshold: 3,
            recovery_threshold: 3,
            cooldown_ms: 0,
            ..LoadBalanceConfig::default()
        };
        mon.set_config(&cfg);

        mon.update_device(DevicePathInput {
            ifname: "wlan0",
            connected: true,
            status_reason: "Connected",
            ipv4_address: Some("192.168.1.100/24"),
            ipv4_gateway: Some("192.168.1.1"),
        });

        let p0 = mon.find_path("wlan0").expect("path exists");
        assert_eq!(p0.state, PathState::Probing);
        assert!(!p0.is_eligible);

        // Probe 1 & 2 -> stay Probing
        mon.probe_all_now().await;
        let p1 = mon.find_path("wlan0").expect("p1");
        assert_eq!(p1.state, PathState::Probing);
        assert_eq!(p1.consecutive_successes, 1);
        assert!(!p1.is_eligible);

        mon.probe_all_now().await;
        let p2 = mon.find_path("wlan0").expect("p2");
        assert_eq!(p2.state, PathState::Probing);
        assert_eq!(p2.consecutive_successes, 2);
        assert!(!p2.is_eligible);

        // Probe 3 -> transitions to Online
        mon.probe_all_now().await;
        let p3 = mon.find_path("wlan0").expect("p3");
        assert_eq!(p3.state, PathState::Online);
        assert_eq!(p3.consecutive_successes, 3);
        assert!(p3.is_eligible);

        // Probe 4 & 5 fail -> hysteresis keeps Online
        next_success.store(false, Ordering::Relaxed);
        mon.probe_all_now().await;
        let p4 = mon.find_path("wlan0").expect("p4");
        assert_eq!(p4.state, PathState::Online);
        assert_eq!(p4.consecutive_failures, 1);
        assert!(p4.is_eligible);

        mon.probe_all_now().await;
        let p5 = mon.find_path("wlan0").expect("p5");
        assert_eq!(p5.state, PathState::Online);
        assert_eq!(p5.consecutive_failures, 2);

        // Probe 6 fails -> LocalOnly + ineligible
        mon.probe_all_now().await;
        let p6 = mon.find_path("wlan0").expect("p6");
        assert_eq!(p6.state, PathState::LocalOnly);
        assert_eq!(p6.consecutive_failures, 3);
        assert!(!p6.is_eligible);
        assert_eq!(calls.load(Ordering::Relaxed), 6);
    }

    #[tokio::test]
    async fn device_disconnect_removal_and_manual_update() {
        let mon = PathMonitor::new();
        mon.update_device(DevicePathInput {
            ifname: "wlan1",
            connected: true,
            status_reason: "Connected",
            ipv4_address: Some("10.0.0.5/24"),
            ipv4_gateway: Some("10.0.0.1"),
        });

        mon.update_device(DevicePathInput {
            ifname: "wlan1",
            connected: false,
            status_reason: "Disconnected",
            ipv4_address: None,
            ipv4_gateway: None,
        });

        let p = mon.find_path("wlan1").expect("offline path");
        assert_eq!(p.state, PathState::Offline);
        assert!(!p.is_eligible);

        mon.remove_device("wlan1");
        assert!(mon.find_path("wlan1").is_none());

        let mut manual = PathInfo::new("wlp0s20f0u1");
        manual.state = PathState::Online;
        manual.rtt_ms = 4.2;
        manual.is_eligible = true;
        mon.update_path_info(&manual);

        let got = mon.find_path("wlp0s20f0u1").expect("manual");
        assert_eq!(got.state, PathState::Online);
        assert!((got.rtt_ms - 4.2).abs() < 1e-6);
        assert!(got.is_eligible);
    }

    #[tokio::test]
    async fn nonblocking_port_probe_detects_listener_and_closed_port() {
        let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let port = listener.local_addr().expect("addr").port();

        assert!(try_single_port_probe("", Some("127.0.0.1"), Ipv4Addr::LOCALHOST, port, 250).await);

        drop(listener);
        // Closed port returns ECONNREFUSED on loopback, proving reachability per C behavior
        assert!(try_single_port_probe("", Some("127.0.0.1"), Ipv4Addr::LOCALHOST, port, 250).await);
    }
}
