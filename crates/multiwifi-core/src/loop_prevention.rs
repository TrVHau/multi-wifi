#![cfg_attr(not(test), deny(unsafe_code))]

use crate::netlink_route::{parse_ipv4_clean, NetlinkError, NetlinkSession};
use thiserror::Error;

const MAIN_TABLE_ID: u32 = 254;
const DEFAULT_HOST_ROUTE_METRIC: u32 = 10;

#[derive(Debug, Error)]
pub enum LoopPreventionError {
    #[error("incomplete loop prevention configuration")]
    InvalidConfig,

    #[error("netlink error in loop prevention: {0}")]
    Netlink(#[from] NetlinkError),
}

/// Pins a `/32` host route to the bonding relay server via the physical uplink
/// so tunnel traffic itself is never captured inside `mw-tun0`.
#[derive(Debug, Clone)]
pub struct LoopPrevention {
    server_ip: String,
    physical_gw: String,
    physical_ifname: String,
    metric: u32,
    active: bool,
}

impl LoopPrevention {
    #[must_use]
    pub fn new(server_ip: &str, physical_gw: &str, physical_ifname: &str, metric: u32) -> Self {
        Self {
            server_ip: server_ip.to_owned(),
            physical_gw: physical_gw.to_owned(),
            physical_ifname: physical_ifname.to_owned(),
            metric: if metric > 0 {
                metric
            } else {
                DEFAULT_HOST_ROUTE_METRIC
            },
            active: false,
        }
    }

    pub async fn apply(&mut self) -> Result<(), LoopPreventionError> {
        if self.server_ip.is_empty()
            || self.physical_gw.is_empty()
            || self.physical_ifname.is_empty()
        {
            return Err(LoopPreventionError::InvalidConfig);
        }

        let ip = parse_ipv4_clean(&self.server_ip)?;
        if ip.is_loopback() {
            // Loopback destinations (127.0.0.0/8) must never be pinned to a physical Wi-Fi gateway.
            self.active = false;
            return Ok(());
        }

        let nl = NetlinkSession::open()?;
        nl.add_host_route(
            MAIN_TABLE_ID,
            &self.server_ip,
            &self.physical_gw,
            &self.physical_ifname,
            self.metric,
        )
        .await?;
        self.active = true;
        Ok(())
    }

    pub async fn rollback(&mut self) -> Result<(), LoopPreventionError> {
        if !self.active {
            return Ok(());
        }
        let nl = NetlinkSession::open()?;
        let res = nl
            .del_host_route(
                MAIN_TABLE_ID,
                &self.server_ip,
                &self.physical_gw,
                &self.physical_ifname,
            )
            .await;
        self.active = false;
        res?;
        Ok(())
    }

    #[must_use]
    pub const fn is_active(&self) -> bool {
        self.active
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn loopback_target_skips_host_route() {
        let mut lp = LoopPrevention::new("127.0.0.1", "192.168.1.1", "lo", 10);
        lp.apply().await.expect("loopback skips cleanly");
        assert!(!lp.is_active());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn netns_host_route_pin_and_rollback() {
        use crate::netlink_route::test_util::{run_in_isolated_netns, setup_loopback};
        use std::net::Ipv4Addr;

        run_in_isolated_netns(|| {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("rt");
            rt.block_on(async {
                setup_loopback(&[(Ipv4Addr::new(192, 168, 10, 2), 24)]).await;

                let mut lp = LoopPrevention::new("203.0.113.50", "192.168.10.1", "lo", 10);
                lp.apply().await.expect("apply host route");
                assert!(lp.is_active());

                let nl = NetlinkSession::open().expect("open");
                assert!(nl
                    .verify_route(MAIN_TABLE_ID, Some("192.168.10.1"), Some("lo"))
                    .await
                    .expect("verify host route"));

                lp.rollback().await.expect("rollback host route");
                assert!(!lp.is_active());
            });
        });
    }
}
