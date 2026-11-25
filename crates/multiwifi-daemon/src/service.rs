#![deny(unsafe_code)]

use crate::dns::{
    apply_dns_configuration, is_custom_dns_active, notify_nm_reload_async,
    read_current_dns_servers, reset_dns_configuration, DnsError, DnsPaths,
};
use crate::errors::DaemonError;
use crate::nm_client::{NmInventory, WifiDeviceState};
use crate::operation::{insert_val, OpManager, OpState};
use crate::polkit::{BusKind, PolkitAuthorizer};
use multiwifi_core::bonding_client::{BondingClient, BondingProfile, BondingState, BondingUplink};
use multiwifi_core::capabilities::Capabilities;
use multiwifi_core::config::{LoadBalanceStrategy, MultiWifiConfig};
use multiwifi_core::firewall::FirewallManager;
use multiwifi_core::load_balancer::{Scheduler, UplinkState};
use multiwifi_core::path_monitor::{PathInfo, PathMonitor};
use multiwifi_core::routing_manager::{RoutePlan, RoutingManager};
use nix::unistd::geteuid;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, Instant};
use tokio::sync::{watch, Mutex as AsyncMutex};
use tokio::task::JoinHandle;
use zbus::connection::Builder as ConnBuilder;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::{interface, Connection, OwnedVariant};

pub const DAEMON_VERSION: &str = env!("CARGO_PKG_VERSION");
pub const DAEMON_API_VERSION: u32 = 1;
pub const DBUS_NAME: &str = "io.github.multiwifi.Daemon";
pub const DBUS_PATH: &str = "/io/github/multiwifi/Daemon";

const DIAGNOSTICS_COOLDOWN: Duration = Duration::from_millis(1500);
const DEFAULT_JOURNAL_DIR: &str = "/run/multiwifi/journals";
const DEFAULT_CONFIG_PATH: &str = "/etc/multiwifi/multiwifi.conf";

#[derive(Debug, Clone)]
pub struct ServiceConfig {
    pub bus_kind: BusKind,
    pub enable_polkit: bool,
    pub bus_name: String,
    pub dns_paths: Option<DnsPaths>,
    pub journal_dir: Option<PathBuf>,
    pub config_path: Option<PathBuf>,
}

impl ServiceConfig {
    #[must_use]
    pub fn production(bus_kind: BusKind, enable_polkit: bool) -> Self {
        let journal_dir = if geteuid().is_root() {
            Some(PathBuf::from(DEFAULT_JOURNAL_DIR))
        } else {
            None
        };
        let config_path = std::env::var_os("MULTIWIFI_CONFIG_PATH")
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .or_else(|| Some(PathBuf::from(DEFAULT_CONFIG_PATH)));
        Self {
            bus_kind,
            enable_polkit,
            bus_name: DBUS_NAME.to_owned(),
            dns_paths: None,
            journal_dir,
            config_path,
        }
    }
}

pub struct DaemonCore {
    config: ServiceConfig,
    polkit: PolkitAuthorizer,
    inventory: NmInventory,
    path_monitor: PathMonitor,
    op_manager: OpManager,
    routing_mgr: RoutingManager,
    scheduler: Scheduler,
    firewall: FirewallManager,
    bonding_client: BondingClient,
    last_applied_marks: Mutex<Vec<u32>>,
    custom_weights: Mutex<HashMap<String, u32>>,
    disabled_uplinks: Mutex<HashSet<String>>,
    last_diagnostics_at: Mutex<Option<Instant>>,
    custom_dns_applied: Mutex<bool>,
    shutdown_tx: watch::Sender<bool>,
}

impl DaemonCore {
    #[must_use]
    pub fn new(config: ServiceConfig) -> Arc<Self> {
        let (shutdown_tx, _) = watch::channel(false);
        let journal_dir = config
            .journal_dir
            .clone()
            .unwrap_or_else(|| std::env::temp_dir().join("multiwifi_journals"));
        let polkit = PolkitAuthorizer::new(config.enable_polkit, config.bus_kind);

        Arc::new(Self {
            config,
            polkit,
            inventory: NmInventory::new(),
            path_monitor: PathMonitor::new(),
            op_manager: OpManager::new(),
            routing_mgr: RoutingManager::new(journal_dir),
            scheduler: Scheduler::new(LoadBalanceStrategy::WeightedRoundRobin),
            firewall: FirewallManager::new(),
            bonding_client: BondingClient::new(),
            last_applied_marks: Mutex::new(Vec::new()),
            custom_weights: Mutex::new(HashMap::new()),
            disabled_uplinks: Mutex::new(HashSet::new()),
            last_diagnostics_at: Mutex::new(None),
            custom_dns_applied: Mutex::new(false),
            shutdown_tx,
        })
    }

    pub async fn recover_startup_state(&self) {
        if let Some(ref cfg_path) = self.config.config_path {
            self.apply_runtime_config_file(cfg_path);
        }
        let _ = self.routing_mgr.recover().await;
        if let Ok(true) = self.firewall.verify() {
            let _ = self.firewall.teardown();
        }
    }

    fn apply_runtime_config_file(&self, path: &Path) {
        match std::fs::symlink_metadata(path) {
            Ok(_) => {}
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return,
            Err(err) => {
                eprintln!(
                    "Unable to inspect MultiWiFi config '{}': {err}",
                    path.display()
                );
                return;
            }
        }

        match MultiWifiConfig::parse_file(path) {
            Ok(cfg) => {
                self.path_monitor.set_config(&cfg.load_balance);
                let mut weights = self
                    .custom_weights
                    .lock()
                    .unwrap_or_else(PoisonError::into_inner);
                let mut disabled = self
                    .disabled_uplinks
                    .lock()
                    .unwrap_or_else(PoisonError::into_inner);
                for up in &cfg.uplinks {
                    weights.insert(up.device_id.clone(), up.weight);
                    if up.enabled {
                        disabled.remove(&up.device_id);
                    } else {
                        disabled.insert(up.device_id.clone());
                    }
                }
            }
            Err(err) => {
                eprintln!(
                    "Ignoring invalid MultiWiFi config '{}': {err}",
                    path.display()
                );
            }
        }
    }

    #[must_use]
    pub fn subscribe_shutdown(&self) -> watch::Receiver<bool> {
        self.shutdown_tx.subscribe()
    }

    fn effective_dns_paths(&self) -> DnsPaths {
        self.config
            .dns_paths
            .clone()
            .unwrap_or_else(DnsPaths::from_env)
    }

    async fn build_status_dict(&self) -> HashMap<String, OwnedVariant> {
        let routing_active = self.routing_mgr.is_active().await;
        let fw_active = self.firewall.is_active();
        let bstate = self.bonding_client.state();
        let bonding_active = bstate == BondingState::Connected;
        let eligible_paths = u32::try_from(self.scheduler.eligible_count()).unwrap_or(u32::MAX);

        let mode_str = if routing_active {
            "load_balancing"
        } else if bonding_active {
            "bonding"
        } else {
            "idle"
        };

        let mut map = HashMap::new();
        insert_val(&mut map, "version", DAEMON_VERSION);
        insert_val(&mut map, "mode", mode_str);
        insert_val(&mut map, "daemon_running", true);
        insert_val(&mut map, "routing_active", routing_active);
        insert_val(&mut map, "firewall_active", fw_active);
        insert_val(&mut map, "bonding_active", bonding_active);
        insert_val(&mut map, "bonding_state", bstate.as_str());
        insert_val(&mut map, "bonding_mptcp", self.bonding_client.is_mptcp());
        insert_val(
            &mut map,
            "bonding_virtual_ip",
            self.bonding_client.virtual_ip().unwrap_or_default(),
        );
        insert_val(&mut map, "eligible_uplinks", eligible_paths);
        insert_val(&mut map, "polkit_enforced", self.polkit.is_enforced());
        map
    }

    async fn emit_op_progress(
        &self,
        emitter: &SignalEmitter<'_>,
        op_id: &str,
        state: OpState,
        percent: u32,
        msg: &str,
    ) {
        self.op_manager.update(op_id, state, percent, msg);
        let _ = DaemonInterface::operation_progress(emitter, op_id, percent, msg).await;
    }

    async fn emit_op_finished(
        &self,
        emitter: &SignalEmitter<'_>,
        op_id: &str,
        success: bool,
        result: Option<HashMap<String, OwnedVariant>>,
        msg: &str,
    ) {
        let payload = result.unwrap_or_default();
        self.op_manager
            .finish(op_id, success, Some(payload.clone()), Some(msg));
        let _ = DaemonInterface::operation_finished(emitter, op_id, success, payload).await;
    }

    async fn start_load_balancing_impl(
        &self,
        emitter: &SignalEmitter<'_>,
    ) -> Result<String, DaemonError> {
        if self.bonding_client.state() == BondingState::Connected {
            return Err(DaemonError::Conflict(
                "Cannot start load balancing while bonding is active".to_owned(),
            ));
        }

        if self.routing_mgr.is_active().await {
            let op_id = self.op_manager.create_op("StartLoadBalancing");
            self.emit_op_finished(
                emitter,
                &op_id,
                true,
                None,
                "Load balancing is already active",
            )
            .await;
            let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
            return Ok(op_id);
        }

        let op_id = self.op_manager.create_op("StartLoadBalancing");
        let devices = self.inventory.refresh().await;
        let disabled_set: HashSet<String> = self
            .disabled_uplinks
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone();
        let mut plan = RoutePlan::new();
        let mut idx = 0u32;
        for d in &devices {
            if disabled_set.contains(&d.ifname) {
                continue;
            }
            if d.state == WifiDeviceState::Connected
                && !d.ipv4_address.is_empty()
                && !d.ipv4_gateway.is_empty()
            {
                let ifindex = u32::try_from(d.ifindex).unwrap_or(0);
                plan.add_uplink(
                    &d.ifname,
                    ifindex,
                    Some(&d.ipv4_address),
                    &d.ipv4_gateway,
                    idx,
                );
                idx = idx.saturating_add(1);
            }
        }

        if plan.uplinks.len() < 2 {
            let count = plan.uplinks.len();
            self.emit_op_finished(
                emitter,
                &op_id,
                false,
                None,
                "Load balancing requires at least 2 connected Wi-Fi interfaces with IPv4 default gateways",
            )
            .await;
            return Err(DaemonError::NetworkFailure(format!(
                "Cần tối thiểu 2 kết nối Wi-Fi hoạt động để thực hiện Cân bằng tải (hiện có {count})"
            )));
        }

        self.emit_op_progress(
            emitter,
            &op_id,
            OpState::Applying,
            40,
            "Applying multi-path routing tables",
        )
        .await;

        if let Err(err) = self.routing_mgr.apply_plan(&op_id, &mut plan).await {
            let msg = err.to_string();
            self.emit_op_finished(emitter, &op_id, false, None, &msg)
                .await;
            return Err(DaemonError::Conflict(msg));
        }

        self.apply_scheduler_and_firewall(emitter, &op_id, &plan)
            .await?;
        self.emit_op_finished(emitter, &op_id, true, None, "Load balancing started")
            .await;
        let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
        Ok(op_id)
    }

    async fn apply_scheduler_and_firewall(
        &self,
        emitter: &SignalEmitter<'_>,
        op_id: &str,
        plan: &RoutePlan,
    ) -> Result<(), DaemonError> {
        self.scheduler.reset();
        for up in &plan.uplinks {
            let path = self.path_monitor.find_path(&up.ifname);
            let eligible = path.as_ref().is_none_or(|p| p.is_eligible);
            let rtt_ms = path.as_ref().map_or(10.0, |p| p.rtt_ms);
            let loss_percent = path.as_ref().map_or(0, |p| p.packet_loss_percent);
            let configured_weight = self
                .custom_weights
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .get(&up.ifname)
                .copied()
                .filter(|&w| w > 0)
                .unwrap_or(100);

            self.scheduler.add_or_update(&UplinkState {
                device_id: &up.ifname,
                ifname: &up.ifname,
                fwmark: up.fwmark,
                weight: configured_weight,
                is_eligible: eligible,
                rtt_ms,
                loss_percent,
            });
        }

        let marks = self.scheduler.distribution();
        if marks.is_empty() {
            let _ = self.routing_mgr.teardown().await;
            self.emit_op_finished(
                emitter,
                op_id,
                false,
                None,
                "No eligible uplinks available for load balancing",
            )
            .await;
            return Err(DaemonError::NetworkFailure(
                "No eligible uplinks available for load balancing".to_owned(),
            ));
        }

        if let Err(err) = self.firewall.apply(&marks) {
            let _ = self.routing_mgr.teardown().await;
            let msg = err.to_string();
            self.emit_op_finished(emitter, op_id, false, None, &msg)
                .await;
            return Err(DaemonError::NetworkFailure(msg));
        }

        *self
            .last_applied_marks
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = marks;
        Ok(())
    }

    async fn stop_load_balancing_impl(
        &self,
        emitter: &SignalEmitter<'_>,
    ) -> Result<String, DaemonError> {
        if !self.routing_mgr.is_active().await {
            let op_id = self.op_manager.create_op("StopLoadBalancing");
            self.emit_op_finished(
                emitter,
                &op_id,
                true,
                None,
                "Load balancing is already inactive",
            )
            .await;
            let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
            return Ok(op_id);
        }

        let op_id = self.op_manager.create_op("StopLoadBalancing");
        let _ = self.firewall.teardown();
        self.last_applied_marks
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clear();
        self.scheduler.reset();

        if let Err(err) = self.routing_mgr.teardown().await {
            let msg = err.to_string();
            self.emit_op_finished(emitter, &op_id, false, None, &msg)
                .await;
            return Err(DaemonError::RollbackFailed(msg));
        }

        self.emit_op_finished(emitter, &op_id, true, None, "Load balancing stopped")
            .await;
        let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
        Ok(op_id)
    }

    async fn start_bonding_impl(
        &self,
        emitter: &SignalEmitter<'_>,
        profile_dict: &HashMap<String, OwnedVariant>,
    ) -> Result<String, DaemonError> {
        if self.routing_mgr.is_active().await {
            return Err(DaemonError::Conflict(
                "Cannot start bonding while load balancing is active".to_owned(),
            ));
        }

        let bstate = self.bonding_client.state();
        if bstate == BondingState::Connected {
            let op_id = self.op_manager.create_op("StartBonding");
            self.emit_op_finished(
                emitter,
                &op_id,
                true,
                None,
                "Bonding tunnel is already active",
            )
            .await;
            let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
            return Ok(op_id);
        }
        if bstate != BondingState::Disconnected && bstate != BondingState::Failed {
            return Err(DaemonError::Conflict(
                "Bonding tunnel state transition is already in progress".to_owned(),
            ));
        }

        let server_host = dict_str(profile_dict, "server_host")
            .unwrap_or_default()
            .trim()
            .to_owned();
        let token = dict_str(profile_dict, "token")
            .unwrap_or_default()
            .trim()
            .to_owned();
        let server_port = dict_port(profile_dict).unwrap_or(5000);
        let profile_id = dict_str(profile_dict, "profile_id")
            .map(|s| s.trim().to_owned())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "default".to_owned());

        if server_host.is_empty() || token.is_empty() || server_port == 0 || server_port > 65535 {
            return Err(DaemonError::InvalidArgument(
                "Invalid bonding profile: server_host, token, and valid server_port (1-65535) are required"
                    .to_owned(),
            ));
        }

        let op_id = self.op_manager.create_op("StartBonding");
        self.emit_op_progress(
            emitter,
            &op_id,
            OpState::Validating,
            20,
            "Resolving host and verifying network uplinks",
        )
        .await;

        let uplink = self
            .inventory
            .devices()
            .into_iter()
            .find(|d| d.state == WifiDeviceState::Connected && !d.ipv4_gateway.is_empty())
            .map(|d| BondingUplink {
                physical_ifname: d.ifname,
                physical_gw: d.ipv4_gateway,
            });
        self.bonding_client.set_uplink(uplink);
        self.bonding_client.set_profile(BondingProfile {
            profile_id,
            server_host,
            server_port: u16::try_from(server_port).unwrap_or(5000),
            auth_token: token,
        });

        self.emit_op_progress(
            emitter,
            &op_id,
            OpState::Applying,
            50,
            "Connecting to aggregation server and establishing TUN",
        )
        .await;

        if let Err(err) = self.bonding_client.start().await {
            let msg = err.to_string();
            self.emit_op_finished(emitter, &op_id, false, None, &msg)
                .await;
            return Err(DaemonError::NetworkFailure(msg));
        }

        self.emit_op_finished(
            emitter,
            &op_id,
            true,
            None,
            "Bonding tunnel established successfully",
        )
        .await;
        let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
        Ok(op_id)
    }

    async fn stop_bonding_impl(&self, emitter: &SignalEmitter<'_>) -> String {
        if self.bonding_client.state() == BondingState::Disconnected {
            let op_id = self.op_manager.create_op("StopBonding");
            self.emit_op_finished(
                emitter,
                &op_id,
                true,
                None,
                "Bonding tunnel is already inactive",
            )
            .await;
            let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
            return op_id;
        }

        let op_id = self.op_manager.create_op("StopBonding");
        self.emit_op_progress(
            emitter,
            &op_id,
            OpState::Applying,
            50,
            "Tearing down bonding tunnel and restoring routes",
        )
        .await;
        self.bonding_client.stop().await;
        self.emit_op_finished(
            emitter,
            &op_id,
            true,
            None,
            "Bonding tunnel terminated cleanly",
        )
        .await;
        let _ = DaemonInterface::status_changed(emitter, self.build_status_dict().await).await;
        op_id
    }

    async fn on_paths_updated(&self, conn: &Connection, paths: Vec<PathInfo>) {
        if let Ok(iface_ref) = conn
            .object_server()
            .interface::<_, DaemonInterface>(DBUS_PATH)
            .await
        {
            let emitter = iface_ref.signal_emitter();
            for p in &paths {
                let _ = DaemonInterface::path_changed(emitter, path_to_dbus_dict(p)).await;
                if self.routing_mgr.is_active().await {
                    let fwmark = self.routing_mgr.fwmark_for(&p.ifname).await;
                    if fwmark > 0 {
                        self.scheduler.add_or_update(&UplinkState {
                            device_id: &p.ifname,
                            ifname: &p.ifname,
                            fwmark,
                            weight: 0,
                            is_eligible: p.is_eligible,
                            rtt_ms: p.rtt_ms,
                            loss_percent: p.packet_loss_percent,
                        });
                    }
                }
            }
        }

        if self.routing_mgr.is_active().await {
            let marks = self.scheduler.distribution();
            if marks.is_empty() {
                if self.firewall.is_active() {
                    let _ = self.firewall.teardown();
                    self.last_applied_marks
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner)
                        .clear();
                }
            } else {
                let changed = {
                    let last = self
                        .last_applied_marks
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner);
                    !self.firewall.is_active() || *last != marks
                };
                if changed && self.firewall.apply(&marks).is_ok() {
                    *self
                        .last_applied_marks
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner) = marks;
                }
            }
        }
    }

    pub async fn shutdown(&self) {
        self.inventory.stop();
        self.path_monitor.stop().await;

        let dns_was_applied = {
            let mut applied = self
                .custom_dns_applied
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            let prev = *applied;
            *applied = false;
            prev
        };
        if dns_was_applied {
            let _ = reset_dns_configuration(&self.effective_dns_paths());
        }

        if self.firewall.is_active() {
            let _ = self.firewall.teardown();
        }
        self.last_applied_marks
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clear();

        if self.routing_mgr.is_active().await {
            let _ = self.routing_mgr.teardown().await;
        }
        if self.bonding_client.state() != BondingState::Disconnected {
            self.bonding_client.stop().await;
        }
    }
}

pub struct DaemonInterface {
    core: Arc<DaemonCore>,
}

#[interface(name = "io.github.multiwifi.Daemon1")]
impl DaemonInterface {
    #[zbus(name = "GetVersion", out_args("version", "api_version"))]
    fn get_version(&self) -> (String, u32) {
        let _ = &self.core;
        (DAEMON_VERSION.to_owned(), DAEMON_API_VERSION)
    }

    #[zbus(name = "GetCapabilities", out_args("capabilities"))]
    async fn get_capabilities(&self) -> HashMap<String, OwnedVariant> {
        let caps = Capabilities::probe_async().await;
        let mut map = HashMap::new();
        insert_val(&mut map, "kernel_version", caps.kernel_version);
        insert_val(&mut map, "mptcp_state", caps.kernel_mptcp.as_str());
        insert_val(&mut map, "mptcp_details", caps.mptcp_details);
        insert_val(&mut map, "nftables_state", caps.nftables.as_str());
        insert_val(&mut map, "tun_state", caps.tun_device.as_str());
        insert_val(
            &mut map,
            "network_manager_state",
            caps.network_manager.as_str(),
        );
        map
    }

    #[zbus(name = "GetDevices", out_args("devices"))]
    async fn get_devices(&self) -> Vec<HashMap<String, OwnedVariant>> {
        self.core
            .inventory
            .refresh()
            .await
            .iter()
            .map(crate::nm_client::WifiDeviceInfo::to_dbus_dict)
            .collect()
    }

    #[zbus(name = "GetPaths", out_args("paths"))]
    fn get_paths(&self) -> Vec<HashMap<String, OwnedVariant>> {
        self.core
            .path_monitor
            .paths()
            .iter()
            .map(path_to_dbus_dict)
            .collect()
    }

    #[zbus(name = "GetStatus", out_args("status"))]
    async fn get_status(&self) -> HashMap<String, OwnedVariant> {
        self.core.build_status_dict().await
    }

    #[zbus(name = "StartLoadBalancing", out_args("operation_id"))]
    async fn start_load_balancing(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        options: HashMap<String, OwnedVariant>,
    ) -> Result<String, DaemonError> {
        let _ = options;
        self.core.polkit.authorize(conn, &header).await?;
        self.core.start_load_balancing_impl(&emitter).await
    }

    #[zbus(name = "StopLoadBalancing", out_args("operation_id"))]
    async fn stop_load_balancing(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> Result<String, DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        self.core.stop_load_balancing_impl(&emitter).await
    }

    #[zbus(name = "StartBonding", out_args("operation_id"))]
    async fn start_bonding(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        profile: HashMap<String, OwnedVariant>,
    ) -> Result<String, DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        self.core.start_bonding_impl(&emitter, &profile).await
    }

    #[zbus(name = "StopBonding", out_args("operation_id"))]
    async fn stop_bonding(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> Result<String, DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        Ok(self.core.stop_bonding_impl(&emitter).await)
    }

    #[zbus(name = "SetPathWeight")]
    async fn set_path_weight(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        device_id: &str,
        weight: u32,
    ) -> Result<(), DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        if device_id.is_empty() || !(1..=1000).contains(&weight) {
            return Err(DaemonError::InvalidArgument(
                "Invalid parameters: weight must be between 1 and 1000 and device_id must be non-empty"
                    .to_owned(),
            ));
        }

        self.core
            .custom_weights
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(device_id.to_owned(), weight);
        self.core.scheduler.set_weight(device_id, weight);

        if self.core.firewall.is_active() && self.core.routing_mgr.is_active().await {
            let marks = self.core.scheduler.distribution();
            if !marks.is_empty() {
                let changed = *self
                    .core
                    .last_applied_marks
                    .lock()
                    .unwrap_or_else(PoisonError::into_inner)
                    != marks;
                if changed && self.core.firewall.apply(&marks).is_ok() {
                    *self
                        .core
                        .last_applied_marks
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner) = marks;
                }
            }
        }
        Ok(())
    }

    #[zbus(name = "RunDiagnostics", out_args("operation_id"))]
    async fn run_diagnostics(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> Result<String, DaemonError> {
        {
            let mut last = self
                .core
                .last_diagnostics_at
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            let now = Instant::now();
            if let Some(prev) = *last {
                if now.saturating_duration_since(prev) < DIAGNOSTICS_COOLDOWN {
                    return Err(DaemonError::Busy(
                        "Diagnostics probe already in progress or ran recently. Please wait."
                            .to_owned(),
                    ));
                }
            }
            *last = Some(now);
        }

        let op_id = self.core.op_manager.create_op("RunDiagnostics");
        self.core
            .emit_op_progress(
                &emitter,
                &op_id,
                OpState::Applying,
                50,
                "Executing probe cycle",
            )
            .await;

        let monitor = self.core.path_monitor.clone();
        tokio::spawn(async move {
            monitor.probe_all_now().await;
        });

        self.core
            .emit_op_finished(&emitter, &op_id, true, None, "Diagnostics cycle completed")
            .await;
        Ok(op_id)
    }

    #[zbus(name = "GetDiagnostics", out_args("result"))]
    fn get_diagnostics(
        &self,
        operation_id: &str,
    ) -> Result<HashMap<String, OwnedVariant>, DaemonError> {
        self.core
            .op_manager
            .to_dbus_dict(operation_id)
            .ok_or_else(|| {
                DaemonError::InvalidArgument("Unknown or expired operation ID".to_owned())
            })
    }

    #[zbus(name = "StopDaemon")]
    async fn stop_daemon(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<(), DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        let _ = self.core.shutdown_tx.send(true);
        Ok(())
    }

    #[zbus(name = "GetDNS", out_args("servers", "is_custom"))]
    fn get_dns(&self) -> (Vec<String>, bool) {
        let paths = self.core.effective_dns_paths();
        (
            read_current_dns_servers(&paths),
            is_custom_dns_active(&paths),
        )
    }

    #[zbus(name = "SetDNS", out_args("result"))]
    async fn set_dns(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
        servers: Vec<String>,
    ) -> Result<String, DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        let paths = self.core.effective_dns_paths();
        apply_dns_configuration(&paths, &servers).map_err(|err| match err {
            DnsError::InvalidArgument(msg) => DaemonError::InvalidArgument(msg),
            DnsError::SymlinkForbidden(_) | DnsError::Io(_) => {
                DaemonError::NetworkFailure(err.to_string())
            }
        })?;
        *self
            .core
            .custom_dns_applied
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = true;

        let conn_clone = conn.clone();
        let is_sys = self.core.config.bus_kind == BusKind::System;
        tokio::spawn(async move {
            notify_nm_reload_async(Some(&conn_clone), is_sys, &paths).await;
        });
        Ok("OK".to_owned())
    }

    #[zbus(name = "ResetDNS", out_args("result"))]
    async fn reset_dns(
        &self,
        #[zbus(connection)] conn: &Connection,
        #[zbus(header)] header: Header<'_>,
    ) -> Result<String, DaemonError> {
        self.core.polkit.authorize(conn, &header).await?;
        let paths = self.core.effective_dns_paths();
        reset_dns_configuration(&paths)
            .map_err(|err| DaemonError::NetworkFailure(err.to_string()))?;
        *self
            .core
            .custom_dns_applied
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = false;

        let conn_clone = conn.clone();
        let is_sys = self.core.config.bus_kind == BusKind::System;
        tokio::spawn(async move {
            notify_nm_reload_async(Some(&conn_clone), is_sys, &paths).await;
        });
        Ok("OK".to_owned())
    }

    #[zbus(signal, name = "DeviceChanged")]
    async fn device_changed(
        emitter: &SignalEmitter<'_>,
        device: HashMap<String, OwnedVariant>,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "PathChanged")]
    async fn path_changed(
        emitter: &SignalEmitter<'_>,
        path: HashMap<String, OwnedVariant>,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "StatusChanged")]
    async fn status_changed(
        emitter: &SignalEmitter<'_>,
        status: HashMap<String, OwnedVariant>,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "OperationProgress")]
    async fn operation_progress(
        emitter: &SignalEmitter<'_>,
        operation_id: &str,
        percent: u32,
        message: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "OperationFinished")]
    async fn operation_finished(
        emitter: &SignalEmitter<'_>,
        operation_id: &str,
        success: bool,
        result: HashMap<String, OwnedVariant>,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "Warning")]
    async fn warning(emitter: &SignalEmitter<'_>, code: &str, message: &str) -> zbus::Result<()>;
}

/// Running handle for the D-Bus daemon service and its background monitor tasks.
pub struct DaemonServer {
    pub connection: Connection,
    pub core: Arc<DaemonCore>,
    forward_task: JoinHandle<()>,
    lifecycle_lock: AsyncMutex<()>,
}

impl DaemonServer {
    pub async fn start(config: ServiceConfig) -> Result<Self, zbus::Error> {
        let core = DaemonCore::new(config.clone());
        core.recover_startup_state().await;

        let iface = DaemonInterface {
            core: Arc::clone(&core),
        };
        let builder = match config.bus_kind {
            BusKind::System => ConnBuilder::system(),
            BusKind::Session => ConnBuilder::session(),
        };
        let conn = builder
            .name(config.bus_name.as_str())
            .serve_at(DBUS_PATH, iface)
            .build()
            .await?;

        let is_system = config.bus_kind == BusKind::System;
        core.inventory.start(core.path_monitor.clone(), is_system);
        core.path_monitor.start();

        let core_bg = Arc::clone(&core);
        let conn_bg = conn.clone();
        let mut dev_rx = core.inventory.subscribe();
        let mut path_rx = core.path_monitor.subscribe();
        let forward_task = tokio::spawn(async move {
            loop {
                tokio::select! {
                    res = dev_rx.changed() => {
                        if res.is_err() {
                            break;
                        }
                        let devs = dev_rx.borrow().clone();
                        crate::nm_client::sync_devices_to_monitor(&core_bg.path_monitor, &devs);
                        if let Ok(iface_ref) = conn_bg.object_server().interface::<_, DaemonInterface>(DBUS_PATH).await {
                            let emitter = iface_ref.signal_emitter();
                            for d in &devs {
                                let _ = DaemonInterface::device_changed(emitter, d.to_dbus_dict()).await;
                            }
                        }
                    }
                    res = path_rx.changed() => {
                        if res.is_err() {
                            break;
                        }
                        let paths = path_rx.borrow().clone();
                        core_bg.on_paths_updated(&conn_bg, paths).await;
                    }
                }
            }
        });

        Ok(Self {
            connection: conn,
            core,
            forward_task,
            lifecycle_lock: AsyncMutex::new(()),
        })
    }

    pub async fn stop(self) {
        let _guard = self.lifecycle_lock.lock().await;
        self.forward_task.abort();
        let _ = self.forward_task.await;
        self.core.shutdown().await;
        let _ = self
            .connection
            .release_name(self.core.config.bus_name.as_str())
            .await;
        drop(self.connection);
    }
}

fn path_to_dbus_dict(p: &PathInfo) -> HashMap<String, OwnedVariant> {
    let mut map = HashMap::new();
    insert_val(&mut map, "ifname", p.ifname.clone());
    insert_val(&mut map, "gateway", p.gateway.clone().unwrap_or_default());
    insert_val(
        &mut map,
        "source_ip",
        p.source_ip.clone().unwrap_or_default(),
    );
    insert_val(&mut map, "state", p.state.as_str());
    insert_val(&mut map, "status_reason", p.status_reason.clone());
    insert_val(&mut map, "rtt_ms", p.rtt_ms);
    insert_val(&mut map, "min_rtt_ms", p.min_rtt_ms);
    insert_val(&mut map, "max_rtt_ms", p.max_rtt_ms);
    insert_val(&mut map, "jitter_ms", p.jitter_ms);
    insert_val(&mut map, "loss_percent", p.packet_loss_percent);
    insert_val(&mut map, "eligible", p.is_eligible);
    map
}

fn dict_str<'a>(dict: &'a HashMap<String, OwnedVariant>, key: &str) -> Option<&'a str> {
    dict.get(key).and_then(|v| <&str>::try_from(v).ok())
}

fn dict_port(dict: &HashMap<String, OwnedVariant>) -> Option<u32> {
    let v = dict.get("server_port")?;
    if let Ok(p) = u32::try_from(v) {
        return Some(p);
    }
    u16::try_from(v).ok().map(u32::from)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::errors::{
        MULTIWIFI_ERROR_BUSY, MULTIWIFI_ERROR_INVALID_ARGUMENT, MULTIWIFI_ERROR_NOT_AUTHORIZED,
    };
    use futures_util::stream::StreamExt;
    use std::fs;
    use std::os::unix::fs::symlink;
    use zbus::message::Type as MessageType;
    use zbus::{MatchRule, MessageStream, Variant};

    // Note: Keep session-bus integration tests serialized cleanly when sharing the user D-Bus session.
    static SESSION_TEST_LOCK: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

    fn test_config(enable_polkit: bool, dns_paths: Option<DnsPaths>) -> ServiceConfig {
        ServiceConfig {
            bus_kind: BusKind::Session,
            enable_polkit,
            bus_name: DBUS_NAME.to_owned(),
            dns_paths,
            journal_dir: Some(std::env::temp_dir().join("multiwifi_rust_test_journals")),
            config_path: None,
        }
    }

    async fn call_daemon<A, R>(conn: &Connection, method: &str, body: &A) -> zbus::Result<R>
    where
        A: serde::Serialize + zbus::zvariant::DynamicType,
        R: for<'a> serde::Deserialize<'a> + zbus::zvariant::Type,
    {
        let reply = conn
            .call_method(
                Some(DBUS_NAME),
                DBUS_PATH,
                Some(DBUS_NAME_IFACE),
                method,
                body,
            )
            .await?;
        reply.body().deserialize()
    }

    const DBUS_NAME_IFACE: &str = "io.github.multiwifi.Daemon1";

    #[tokio::test]
    async fn dbus_ipc_roundtrip_and_dns() {
        let _guard = SESSION_TEST_LOCK.lock().await;
        let dir = tempfile::tempdir().expect("tempdir");
        let fake_resolv = dir.path().join("resolv.conf");
        fs::write(&fake_resolv, "nameserver 192.168.1.1\n").expect("seed");
        let dns_paths = DnsPaths {
            nm_conf: dir.path().join("99-multiwifi-dns.conf"),
            resolv_conf: fake_resolv,
            resolv_backup: dir.path().join("resolv.conf.bak"),
            has_test_override: true,
        };

        let server = DaemonServer::start(test_config(false, Some(dns_paths)))
            .await
            .expect("start daemon");
        let client = Connection::session().await.expect("session conn");

        // 1. GetVersion
        let (ver, api): (String, u32) = call_daemon(&client, "GetVersion", &())
            .await
            .expect("GetVersion");
        assert_eq!(ver, DAEMON_VERSION);
        assert_eq!(api, DAEMON_API_VERSION);

        // 2. GetCapabilities / GetDevices / GetPaths / GetStatus
        let caps: HashMap<String, OwnedVariant> = call_daemon(&client, "GetCapabilities", &())
            .await
            .expect("GetCapabilities");
        assert!(caps.contains_key("kernel_version"));
        let _: Vec<HashMap<String, OwnedVariant>> = call_daemon(&client, "GetDevices", &())
            .await
            .expect("GetDevices");
        let _: Vec<HashMap<String, OwnedVariant>> = call_daemon(&client, "GetPaths", &())
            .await
            .expect("GetPaths");
        let status: HashMap<String, OwnedVariant> = call_daemon(&client, "GetStatus", &())
            .await
            .expect("GetStatus");
        assert!(status.contains_key("daemon_running"));

        // 3. SetPathWeight validation & happy path
        let err = call_daemon::<_, ()>(&client, "SetPathWeight", &("wlan0", 0u32))
            .await
            .unwrap_err();
        assert!(err.to_string().contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));
        let err_empty = call_daemon::<_, ()>(&client, "SetPathWeight", &("", 10u32))
            .await
            .unwrap_err();
        assert!(err_empty
            .to_string()
            .contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));
        call_daemon::<_, ()>(&client, "SetPathWeight", &("wlan0", 5u32))
            .await
            .expect("valid SetPathWeight");

        // 4. StopLoadBalancing / StopBonding (inactive idempotency)
        let stop_lb_op: String = call_daemon(&client, "StopLoadBalancing", &())
            .await
            .expect("StopLoadBalancing");
        assert_ne!(stop_lb_op, "");
        let stop_bond_op: String = call_daemon(&client, "StopBonding", &())
            .await
            .expect("StopBonding");
        assert_ne!(stop_bond_op, "");

        // 5. RunDiagnostics + GetDiagnostics + rate limit
        let op_id: String = call_daemon(&client, "RunDiagnostics", &())
            .await
            .expect("RunDiagnostics");
        let diag: HashMap<String, OwnedVariant> =
            call_daemon(&client, "GetDiagnostics", &(op_id.as_str(),))
                .await
                .expect("GetDiagnostics");
        assert_eq!(
            diag.get("state").and_then(|v| <&str>::try_from(v).ok()),
            Some("SUCCEEDED")
        );
        let busy = call_daemon::<_, String>(&client, "RunDiagnostics", &())
            .await
            .unwrap_err();
        assert!(busy.to_string().contains(MULTIWIFI_ERROR_BUSY));

        // 6. Invalid GetDiagnostics
        let bad_diag = call_daemon::<_, HashMap<String, OwnedVariant>>(
            &client,
            "GetDiagnostics",
            &("00000000-0000-0000-0000-000000000000",),
        )
        .await
        .unwrap_err();
        assert!(bad_diag
            .to_string()
            .contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));

        verify_dns_dbus_roundtrip(&client).await;
        server.stop().await;
    }

    async fn verify_dns_dbus_roundtrip(client: &Connection) {
        let (dns_init, custom_init): (Vec<String>, bool) =
            call_daemon(client, "GetDNS", &()).await.expect("GetDNS");
        assert!(!custom_init);
        assert_eq!(dns_init, vec!["192.168.1.1"]);

        let bad_dns = call_daemon::<_, String>(client, "SetDNS", &(vec!["bad.ip"],))
            .await
            .unwrap_err();
        assert!(bad_dns
            .to_string()
            .contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));

        let res: String = call_daemon(client, "SetDNS", &(vec!["1.1.1.1", "1.0.0.1"],))
            .await
            .expect("SetDNS");
        assert_eq!(res, "OK");

        let (dns_after, custom_after): (Vec<String>, bool) = call_daemon(client, "GetDNS", &())
            .await
            .expect("GetDNS after");
        assert!(custom_after);
        assert!(dns_after.iter().any(|s| s == "1.1.1.1"));

        let reset_res: String = call_daemon(client, "ResetDNS", &())
            .await
            .expect("ResetDNS");
        assert_eq!(reset_res, "OK");
        let (_, custom_reset): (Vec<String>, bool) = call_daemon(client, "GetDNS", &())
            .await
            .expect("GetDNS reset");
        assert!(!custom_reset);
    }

    #[tokio::test]
    async fn dbus_signals_operation_progress_and_finished() {
        let _guard = SESSION_TEST_LOCK.lock().await;
        let server = DaemonServer::start(test_config(false, None))
            .await
            .expect("start");
        let client = Connection::session().await.expect("session");

        let rule_prog = MatchRule::builder()
            .msg_type(MessageType::Signal)
            .interface(DBUS_NAME_IFACE)
            .path(DBUS_PATH)
            .build();
        let dbus_proxy = zbus::fdo::DBusProxy::new(&client)
            .await
            .expect("dbus proxy");
        dbus_proxy
            .add_match_rule(rule_prog.expect("rule"))
            .await
            .expect("add match");

        let mut stream = MessageStream::from(&client);
        let op_id: String = call_daemon(&client, "RunDiagnostics", &())
            .await
            .expect("RunDiagnostics");

        let mut got_prog = false;
        let mut got_fin = false;
        let wait_fut = tokio::time::timeout(Duration::from_secs(2), async {
            while let Some(msg) = stream.next().await {
                let msg = msg.expect("msg");
                let hdr = msg.header();
                if hdr.message_type() != MessageType::Signal {
                    continue;
                }
                match hdr.member().map(zbus::names::MemberName::as_str) {
                    Some("OperationProgress") => {
                        if let Ok((id, _pct, _msg)) =
                            msg.body().deserialize::<(String, u32, String)>()
                        {
                            if id == op_id {
                                got_prog = true;
                            }
                        }
                    }
                    Some("OperationFinished") => {
                        if let Ok((id, ok, _res)) =
                            msg.body()
                                .deserialize::<(String, bool, HashMap<String, OwnedVariant>)>()
                        {
                            if id == op_id {
                                assert!(ok);
                                got_fin = true;
                            }
                        }
                    }
                    _ => {}
                }
                if got_prog && got_fin {
                    break;
                }
            }
        });
        wait_fut.await.expect("signals timeout");
        assert!(got_prog && got_fin);

        server.stop().await;
    }

    #[tokio::test]
    async fn dbus_polkit_denies_mutating_methods_on_session_bus() {
        let _guard = SESSION_TEST_LOCK.lock().await;
        let server = DaemonServer::start(test_config(true, None))
            .await
            .expect("start");
        let client = Connection::session().await.expect("session");

        let (ver, _): (String, u32) = call_daemon(&client, "GetVersion", &())
            .await
            .expect("read-only allowed");
        assert_eq!(ver, DAEMON_VERSION);

        let empty_opts: HashMap<String, OwnedVariant> = HashMap::new();
        let err_lb = call_daemon::<_, String>(&client, "StartLoadBalancing", &(empty_opts,))
            .await
            .unwrap_err();
        assert!(err_lb.to_string().contains(MULTIWIFI_ERROR_NOT_AUTHORIZED));

        let err_stop = call_daemon::<_, ()>(&client, "StopDaemon", &())
            .await
            .unwrap_err();
        assert!(err_stop
            .to_string()
            .contains(MULTIWIFI_ERROR_NOT_AUTHORIZED));

        let err_setdns = call_daemon::<_, String>(&client, "SetDNS", &(vec!["1.1.1.1"],))
            .await
            .unwrap_err();
        assert!(err_setdns
            .to_string()
            .contains(MULTIWIFI_ERROR_NOT_AUTHORIZED));

        let err_resetdns = call_daemon::<_, String>(&client, "ResetDNS", &())
            .await
            .unwrap_err();
        assert!(err_resetdns
            .to_string()
            .contains(MULTIWIFI_ERROR_NOT_AUTHORIZED));

        server.stop().await;
    }

    #[tokio::test]
    async fn dbus_dns_hardening_rejects_symlinks_and_excess_servers() {
        let _guard = SESSION_TEST_LOCK.lock().await;
        let dir = tempfile::tempdir().expect("tempdir");
        let fake_resolv = dir.path().join("resolv.conf");
        let nm_conf = dir.path().join("99-multiwifi-dns.conf");
        let victim = dir.path().join("victim.txt");
        fs::write(&fake_resolv, "nameserver 8.8.8.8\n").expect("seed");
        symlink(&victim, &nm_conf).expect("symlink");

        let dns_paths = DnsPaths {
            nm_conf: nm_conf.clone(),
            resolv_conf: fake_resolv,
            resolv_backup: dir.path().join("resolv.conf.bak"),
            has_test_override: true,
        };

        let server = DaemonServer::start(test_config(false, Some(dns_paths)))
            .await
            .expect("start");
        let client = Connection::session().await.expect("session");

        let sym_err = call_daemon::<_, String>(&client, "SetDNS", &(vec!["1.1.1.1"],))
            .await
            .unwrap_err();
        assert!(sym_err
            .to_string()
            .contains("io.github.multiwifi.Error.NetworkFailure"));
        assert!(!victim.exists());

        fs::remove_file(&nm_conf).expect("remove symlink");
        let many: Vec<String> = (1..=17).map(|i| format!("1.1.1.{i}")).collect();
        let many_err = call_daemon::<_, String>(&client, "SetDNS", &(many,))
            .await
            .unwrap_err();
        assert!(many_err
            .to_string()
            .contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));

        let mut bad_prof = HashMap::new();
        bad_prof.insert(
            "server_host".to_owned(),
            OwnedVariant::try_from(Variant::from("")).expect("val"),
        );
        let bond_err = call_daemon::<_, String>(&client, "StartBonding", &(bad_prof,))
            .await
            .unwrap_err();
        assert!(bond_err
            .to_string()
            .contains(MULTIWIFI_ERROR_INVALID_ARGUMENT));

        server.stop().await;
    }

    #[tokio::test]
    async fn runtime_config_applies_weights_and_rejects_secrets() {
        let dir = tempfile::tempdir().expect("tempdir");
        let cfg_path = dir.path().join("multiwifi.conf");
        fs::write(
            &cfg_path,
            "\
schema_version = 1
mode = \"load_balance\"

[load_balance]
strategy = \"weighted_round_robin\"
health_check_interval_ms = 1500
failure_threshold = 4
recovery_threshold = 2
cooldown_ms = 3000

[[uplinks]]
device_id = \"wlan0\"
enabled = true
weight = 250

[[uplinks]]
device_id = \"wlan1\"
enabled = false
weight = 50
",
        )
        .expect("write config");

        let mut cfg = test_config(false, None);
        cfg.config_path = Some(cfg_path.clone());
        let core = DaemonCore::new(cfg);
        core.recover_startup_state().await;

        assert_eq!(
            core.custom_weights
                .lock()
                .expect("weights lock")
                .get("wlan0")
                .copied(),
            Some(250)
        );
        assert!(core
            .disabled_uplinks
            .lock()
            .expect("disabled lock")
            .contains("wlan1"));

        let bad_cfg_path = dir.path().join("secret.conf");
        fs::write(
            &bad_cfg_path,
            "schema_version = 1\n[bonding]\nenabled = false\ntoken = \"forbidden\"\n",
        )
        .expect("write bad config");
        let mut bad_cfg = test_config(false, None);
        bad_cfg.config_path = Some(bad_cfg_path);
        let safe_core = DaemonCore::new(bad_cfg);
        safe_core.recover_startup_state().await;
        assert!(safe_core
            .custom_weights
            .lock()
            .expect("weights lock")
            .is_empty());
    }
}
