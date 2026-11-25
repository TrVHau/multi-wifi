#![cfg_attr(not(test), deny(unsafe_code))]

use crate::journal::{JournalEntry, JournalOpType, RouteJournal};
use crate::netlink_route::{parse_ipv4_clean, NetlinkError, NetlinkSession, RuleSpec};
use std::fs;
use std::path::{Path, PathBuf};
use thiserror::Error;
use tokio::sync::Mutex;

pub const BASE_TABLE_ID: u32 = 10_000;
pub const BASE_RULE_PREF: u32 = 20_000;
pub const BASE_FWMARK: u32 = 0x1000;
pub const DEFAULT_FWMASK: u32 = 0xff00;

#[derive(Debug, Error)]
pub enum RoutingError {
    #[error("invalid routing plan: {0}")]
    InvalidArgument(String),

    #[error("routing conflict detected: {0}")]
    Conflict(String),

    #[error("verification failed (rolled back): {0}")]
    VerificationFailed(String),

    #[error("netlink failure: {0}")]
    Netlink(#[from] NetlinkError),

    #[error("journal error: {0}")]
    Journal(#[from] crate::journal::JournalError),

    #[error("filesystem error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UplinkRouteConfig {
    pub ifname: String,
    pub ifindex: u32,
    pub ipv4_address: Option<String>,
    pub gateway: String,
    pub table_id: u32,
    pub src_rule_pref: u32,
    pub fwmark_rule_pref: u32,
    pub fwmark: u32,
    pub fwmask: u32,
    pub metric: u32,
    pub orig_rp_filter: Option<i32>,
}

#[derive(Debug, Clone, Default)]
pub struct RoutePlan {
    pub uplinks: Vec<UplinkRouteConfig>,
}

impl RoutePlan {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add_uplink(
        &mut self,
        ifname: &str,
        ifindex: u32,
        ipv4_addr: Option<&str>,
        gateway: &str,
        index: u32,
    ) {
        self.uplinks.push(UplinkRouteConfig {
            ifname: ifname.to_owned(),
            ifindex,
            ipv4_address: ipv4_addr.filter(|s| !s.is_empty()).map(str::to_owned),
            gateway: gateway.to_owned(),
            table_id: BASE_TABLE_ID + index + 1,
            src_rule_pref: BASE_RULE_PREF + index + 1,
            fwmark_rule_pref: BASE_RULE_PREF + 1000 + index + 1,
            fwmark: BASE_FWMARK + index + 1,
            fwmask: DEFAULT_FWMASK,
            metric: 100 + (index * 10),
            orig_rp_filter: None,
        });
    }
}

#[derive(Debug, Default)]
struct ManagerState {
    active_plan: Option<RoutePlan>,
    is_active: bool,
    rp_filter_all_modified: bool,
    orig_rp_filter_all: i32,
}

/// Transactional Linux policy-routing & RPDB manager with journaled rollback.
#[derive(Debug)]
pub struct RoutingManager {
    journal_dir: PathBuf,
    state: Mutex<ManagerState>,
}

fn is_safe_iface_name(ifname: &str) -> bool {
    !ifname.is_empty() && !ifname.contains('/') && !ifname.contains("..") && !ifname.contains('\0')
}

fn rp_filter_path(ifname: Option<&str>) -> Option<PathBuf> {
    match ifname {
        Some(name) if !name.is_empty() => {
            if !is_safe_iface_name(name) {
                return None;
            }
            Some(PathBuf::from(format!(
                "/proc/sys/net/ipv4/conf/{name}/rp_filter"
            )))
        }
        _ => Some(PathBuf::from("/proc/sys/net/ipv4/conf/all/rp_filter")),
    }
}

fn get_rp_filter(ifname: Option<&str>) -> Option<i32> {
    let path = rp_filter_path(ifname)?;
    let content = fs::read_to_string(path).ok()?;
    content.trim().parse::<i32>().ok()
}

fn set_rp_filter(ifname: Option<&str>, value: i32) {
    let Some(path) = rp_filter_path(ifname) else {
        return;
    };
    let _ = fs::write(path, format!("{value}\n"));
}

fn restore_rp_filters(state: &mut ManagerState, plan: &RoutePlan) {
    for uplink in plan.uplinks.iter().rev() {
        if uplink.orig_rp_filter == Some(1) {
            set_rp_filter(Some(&uplink.ifname), 1);
        }
    }
    if state.rp_filter_all_modified {
        set_rp_filter(None, state.orig_rp_filter_all);
        state.rp_filter_all_modified = false;
    }
}

async fn execute_journal_rollback(nl: &NetlinkSession, journal: &RouteJournal) {
    for entry in journal.entries.iter().rev() {
        match entry.op_type {
            JournalOpType::AddRule => {
                let spec = RuleSpec {
                    table_id: entry.table_id,
                    pref: entry.rule_pref,
                    fwmark: entry.fwmark,
                    fwmask: entry.fwmask,
                    src_ip: entry.src_ip.clone(),
                };
                let _ = nl.del_rule(&spec).await;
            }
            JournalOpType::AddRoute => {
                if let (Some(gw), Some(ifname)) =
                    (entry.gateway.as_deref(), entry.ifname.as_deref())
                {
                    let _ = nl.del_default_route(entry.table_id, gw, ifname).await;
                }
            }
            JournalOpType::AllocTable => {}
        }
    }
}

impl RoutingManager {
    #[must_use]
    pub fn new(journal_dir: impl Into<PathBuf>) -> Self {
        Self {
            journal_dir: journal_dir.into(),
            state: Mutex::new(ManagerState::default()),
        }
    }

    #[must_use]
    pub fn default_journal_dir() -> PathBuf {
        if nix::unistd::Uid::effective().is_root() && Path::new("/run").is_dir() {
            PathBuf::from("/run/multiwifi/journals")
        } else {
            let base =
                std::env::var_os("XDG_RUNTIME_DIR").map_or_else(std::env::temp_dir, PathBuf::from);
            base.join("multiwifi").join("journals")
        }
    }

    pub async fn check_conflicts(&self, plan: &RoutePlan) -> Result<(), RoutingError> {
        if plan.uplinks.is_empty() {
            return Err(RoutingError::InvalidArgument(
                "route plan has no uplinks configured".to_owned(),
            ));
        }

        let nl = NetlinkSession::open()?;
        for u in &plan.uplinks {
            if u.ifname.is_empty() {
                return Err(RoutingError::InvalidArgument(
                    "uplink has empty interface name".to_owned(),
                ));
            }
            if u.gateway.is_empty() {
                return Err(RoutingError::InvalidArgument(format!(
                    "uplink '{}' has no default gateway",
                    u.ifname
                )));
            }
            parse_ipv4_clean(&u.gateway)?;

            if u.table_id <= 255 {
                return Err(RoutingError::Conflict(format!(
                    "table ID {} collides with system reserved range (<= 255)",
                    u.table_id
                )));
            }

            if nl.table_has_routes(u.table_id).await? {
                return Err(RoutingError::Conflict(format!(
                    "table ID {} already contains active routes in kernel",
                    u.table_id
                )));
            }

            if u.ipv4_address.as_deref().is_some_and(|s| !s.is_empty())
                && nl.verify_rule(Some(u.table_id), u.src_rule_pref).await?
            {
                return Err(RoutingError::Conflict(format!(
                    "rule priority {} already exists in RPDB",
                    u.src_rule_pref
                )));
            }

            if u.fwmark > 0 && nl.verify_rule(Some(u.table_id), u.fwmark_rule_pref).await? {
                return Err(RoutingError::Conflict(format!(
                    "fwmark rule priority {} already exists in RPDB",
                    u.fwmark_rule_pref
                )));
            }
        }
        Ok(())
    }

    pub async fn apply_plan(&self, op_id: &str, plan: &mut RoutePlan) -> Result<(), RoutingError> {
        let mut state = self.state.lock().await;

        self.check_conflicts(plan).await?;
        for u in &mut plan.uplinks {
            u.orig_rp_filter = None;
        }

        let nl = NetlinkSession::open()?;
        let mut journal = RouteJournal::new(op_id);
        let journal_path = self.journal_dir.join(format!("{op_id}.journal"));

        let mut apply_err: Option<RoutingError> = None;

        for u in &mut plan.uplinks {
            if let Some(src) = u.ipv4_address.as_deref().filter(|s| !s.is_empty()) {
                let spec = RuleSpec {
                    table_id: u.table_id,
                    pref: u.src_rule_pref,
                    fwmark: 0,
                    fwmask: 0,
                    src_ip: Some(src.to_owned()),
                };
                if let Err(err) = nl.add_rule(&spec).await {
                    apply_err = Some(err.into());
                    break;
                }
                journal.append(JournalEntry::add_rule(
                    u.table_id,
                    u.src_rule_pref,
                    0,
                    0,
                    Some(src),
                    Some(&u.ifname),
                ));
                let _ = journal.save_to_file(&journal_path);
            }

            if u.fwmark > 0 {
                let spec = RuleSpec {
                    table_id: u.table_id,
                    pref: u.fwmark_rule_pref,
                    fwmark: u.fwmark,
                    fwmask: u.fwmask,
                    src_ip: None,
                };
                if let Err(err) = nl.add_rule(&spec).await {
                    apply_err = Some(err.into());
                    break;
                }
                journal.append(JournalEntry::add_rule(
                    u.table_id,
                    u.fwmark_rule_pref,
                    u.fwmark,
                    u.fwmask,
                    None,
                    Some(&u.ifname),
                ));
                let _ = journal.save_to_file(&journal_path);
            }

            if let Err(err) = nl
                .add_default_route(u.table_id, &u.gateway, &u.ifname, u.metric)
                .await
            {
                apply_err = Some(err.into());
                break;
            }
            journal.append(JournalEntry::add_route(
                u.table_id,
                &u.gateway,
                Some(&u.ifname),
                u.metric,
            ));
            let _ = journal.save_to_file(&journal_path);

            u.orig_rp_filter = get_rp_filter(Some(&u.ifname));
            if u.orig_rp_filter == Some(1) {
                set_rp_filter(Some(&u.ifname), 2);
            }
            if get_rp_filter(None) == Some(1) {
                state.orig_rp_filter_all = 1;
                set_rp_filter(None, 2);
                state.rp_filter_all_modified = true;
            }
        }

        if let Some(err) = apply_err {
            execute_journal_rollback(&nl, &journal).await;
            restore_rp_filters(&mut state, plan);
            let _ = RouteJournal::remove_file(&journal_path);
            return Err(err);
        }

        // Verification stage
        let verify_result = self.verify_plan_locked(&nl, plan).await;
        if let Err(err) = verify_result {
            execute_journal_rollback(&nl, &journal).await;
            restore_rp_filters(&mut state, plan);
            let _ = RouteJournal::remove_file(&journal_path);
            return Err(err);
        }

        let _ = RouteJournal::remove_file(&journal_path);
        state.active_plan = Some(plan.clone());
        state.is_active = true;
        Ok(())
    }

    async fn verify_plan_locked(
        &self,
        nl: &NetlinkSession,
        plan: &RoutePlan,
    ) -> Result<(), RoutingError> {
        for u in &plan.uplinks {
            if u.ipv4_address.as_deref().is_some_and(|s| !s.is_empty())
                && !nl.verify_rule(Some(u.table_id), u.src_rule_pref).await?
            {
                return Err(RoutingError::VerificationFailed(format!(
                    "rule pref {} not found in kernel RPDB",
                    u.src_rule_pref
                )));
            }
            if u.fwmark > 0 && !nl.verify_rule(Some(u.table_id), u.fwmark_rule_pref).await? {
                return Err(RoutingError::VerificationFailed(format!(
                    "fwmark rule pref {} not found in kernel RPDB",
                    u.fwmark_rule_pref
                )));
            }
            if !nl
                .verify_route(u.table_id, Some(&u.gateway), Some(&u.ifname))
                .await?
            {
                return Err(RoutingError::VerificationFailed(format!(
                    "default route in table {} via {} dev {} missing",
                    u.table_id, u.gateway, u.ifname
                )));
            }
        }
        Ok(())
    }

    pub async fn teardown(&self) -> Result<(), RoutingError> {
        let mut state = self.state.lock().await;
        let Some(plan) = state.active_plan.take() else {
            state.is_active = false;
            return Ok(());
        };
        state.is_active = false;

        if let Ok(nl) = NetlinkSession::open() {
            for u in plan.uplinks.iter().rev() {
                let _ = nl
                    .del_default_route(u.table_id, &u.gateway, &u.ifname)
                    .await;
                if u.fwmark > 0 {
                    let _ = nl
                        .del_rule(&RuleSpec {
                            table_id: u.table_id,
                            pref: u.fwmark_rule_pref,
                            fwmark: u.fwmark,
                            fwmask: u.fwmask,
                            src_ip: None,
                        })
                        .await;
                }
                if let Some(src) = u.ipv4_address.as_deref().filter(|s| !s.is_empty()) {
                    let _ = nl
                        .del_rule(&RuleSpec {
                            table_id: u.table_id,
                            pref: u.src_rule_pref,
                            fwmark: 0,
                            fwmask: 0,
                            src_ip: Some(src.to_owned()),
                        })
                        .await;
                }
            }
        }
        restore_rp_filters(&mut state, &plan);
        Ok(())
    }

    /// Reconciles and rolls back orphaned journals from prior abnormal exits.
    pub async fn recover(&self) -> Result<(), RoutingError> {
        let _guard = self.state.lock().await;
        if !self.journal_dir.is_dir() {
            return Ok(());
        }

        let nl = NetlinkSession::open()?;
        for entry in fs::read_dir(&self.journal_dir)? {
            let entry = entry?;
            let path = entry.path();
            if path.extension().and_then(|s| s.to_str()) == Some("journal") {
                if let Ok(journal) = RouteJournal::load_from_file(&path) {
                    execute_journal_rollback(&nl, &journal).await;
                }
                let _ = RouteJournal::remove_file(&path);
            }
        }
        Ok(())
    }

    pub async fn is_active(&self) -> bool {
        self.state.lock().await.is_active
    }

    pub async fn fwmark_for(&self, ifname: &str) -> u32 {
        let state = self.state.lock().await;
        if !state.is_active {
            return 0;
        }
        state
            .active_plan
            .as_ref()
            .and_then(|p| p.uplinks.iter().find(|u| u.ifname == ifname))
            .map_or(0, |u| u.fwmark)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn conflict_detection_rejects_bad_plans() {
        let dir = tempfile::tempdir().expect("tempdir");
        let mgr = RoutingManager::new(dir.path());

        let empty = RoutePlan::new();
        assert!(matches!(
            mgr.check_conflicts(&empty).await.unwrap_err(),
            RoutingError::InvalidArgument(_)
        ));

        let mut reserved = RoutePlan::new();
        reserved.add_uplink("lo", 1, Some("192.168.1.2"), "192.168.1.1", 0);
        reserved.uplinks[0].table_id = 254;
        assert!(matches!(
            mgr.check_conflicts(&reserved).await.unwrap_err(),
            RoutingError::Conflict(_)
        ));

        let mut bad_gw = RoutePlan::new();
        bad_gw.add_uplink("lo", 1, Some("192.168.1.2"), "not-an-ip", 0);
        assert!(mgr.check_conflicts(&bad_gw).await.is_err());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn netns_transaction_apply_rollback_and_recovery() {
        use crate::netlink_route::test_util::{run_in_isolated_netns, setup_loopback};
        use std::net::Ipv4Addr;

        run_in_isolated_netns(|| {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("rt");
            rt.block_on(async {
                setup_loopback(&[(Ipv4Addr::new(192, 168, 10, 2), 24)]).await;

                let jdir = tempfile::tempdir().expect("tempdir");
                let mgr = RoutingManager::new(jdir.path());

                // 1. Happy apply -> verify -> teardown
                let mut plan = RoutePlan::new();
                plan.add_uplink("lo", 1, Some("192.168.10.2"), "192.168.10.1", 0);
                mgr.apply_plan("op-good", &mut plan)
                    .await
                    .expect("apply plan");
                assert!(mgr.is_active().await);
                assert_eq!(mgr.fwmark_for("lo").await, 0x1001);

                mgr.teardown().await.expect("teardown");
                assert!(!mgr.is_active().await);
                assert_eq!(mgr.fwmark_for("lo").await, 0);

                // 2. Partial failure rollback
                let mut bad_plan = RoutePlan::new();
                bad_plan.add_uplink("lo", 1, Some("192.168.10.2"), "192.168.10.1", 0);
                bad_plan.add_uplink(
                    "nonexistent_dev",
                    99,
                    Some("192.168.20.2"),
                    "192.168.20.1",
                    1,
                );
                assert!(mgr.apply_plan("op-fail", &mut bad_plan).await.is_err());
                assert!(!mgr.is_active().await);

                let nl = NetlinkSession::open().expect("nl");
                assert!(!nl.verify_rule(Some(10001), 20001).await.expect("verify"));
                assert!(!nl
                    .verify_route(10001, Some("192.168.10.1"), Some("lo"))
                    .await
                    .expect("verify"));

                // 3. Orphaned journal recovery
                let orphan_spec = RuleSpec {
                    table_id: 10005,
                    pref: 20005,
                    fwmark: 0,
                    fwmask: 0,
                    src_ip: Some("192.168.10.2".to_owned()),
                };
                nl.add_rule(&orphan_spec).await.expect("orphan rule");
                nl.add_default_route(10005, "192.168.10.1", "lo", 100)
                    .await
                    .expect("orphan route");

                let mut orphan_j = RouteJournal::new("crash-99");
                orphan_j.append(JournalEntry::add_rule(
                    10005,
                    20005,
                    0,
                    0,
                    Some("192.168.10.2"),
                    Some("lo"),
                ));
                orphan_j.append(JournalEntry::add_route(
                    10005,
                    "192.168.10.1",
                    Some("lo"),
                    100,
                ));
                let jfile = jdir.path().join("crash-99.journal");
                orphan_j.save_to_file(&jfile).expect("save orphan");

                mgr.recover().await.expect("recover");
                assert!(!jfile.exists());
                assert!(!nl.verify_rule(Some(10005), 20005).await.expect("cleaned"));
                assert!(!nl
                    .verify_route(10005, Some("192.168.10.1"), Some("lo"))
                    .await
                    .expect("cleaned"));
            });
        });
    }
}
