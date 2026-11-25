#![cfg_attr(not(test), deny(unsafe_code))]

use futures_util::stream::TryStreamExt;
use netlink_packet_route::route::RouteAddress;
use netlink_packet_route::rule::{RuleAction, RuleAttribute};
use rtnetlink::{Handle, IpVersion, RouteMessageBuilder};
use std::net::Ipv4Addr;
use thiserror::Error;
use tokio::task::JoinHandle;

#[derive(Debug, Error)]
pub enum NetlinkError {
    #[error("invalid netlink argument: {0}")]
    InvalidArgument(String),

    #[error("network interface '{0}' not found")]
    InterfaceNotFound(String),

    #[error("netlink I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("rtnetlink operation failed: {0}")]
    Rtnetlink(#[from] rtnetlink::Error),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuleSpec {
    pub table_id: u32,
    pub pref: u32,
    pub fwmark: u32,
    pub fwmask: u32,
    pub src_ip: Option<String>,
}

/// Manages an active `NETLINK_ROUTE` connection on the Tokio runtime.
pub struct NetlinkSession {
    handle: Handle,
    driver: JoinHandle<()>,
}

impl NetlinkSession {
    /// Opens a `NETLINK_ROUTE` socket and spawns its async driver task.
    pub fn open() -> Result<Self, NetlinkError> {
        let (connection, handle, _) = rtnetlink::new_connection()?;
        let driver = tokio::spawn(connection);
        Ok(Self { handle, driver })
    }

    #[must_use]
    pub fn handle(&self) -> &Handle {
        &self.handle
    }

    pub async fn add_rule(&self, spec: &RuleSpec) -> Result<(), NetlinkError> {
        if spec.table_id == 0 || spec.pref == 0 {
            return Err(NetlinkError::InvalidArgument(
                "table_id and pref must be non-zero".to_owned(),
            ));
        }

        let mut req = self
            .handle
            .rule()
            .add()
            .v4()
            .action(RuleAction::ToTable)
            .priority(spec.pref);

        if spec.table_id > 255 {
            req.message_mut().header.table = 0;
        }
        req = req.table_id(spec.table_id);

        if spec.fwmark > 0 {
            req = req.fw_mark(spec.fwmark);
            if spec.fwmask > 0 {
                req.message_mut()
                    .attributes
                    .push(RuleAttribute::FwMask(spec.fwmask));
            }
        }

        if let Some(src) = spec.src_ip.as_deref().filter(|s| !s.is_empty()) {
            let ip = parse_ipv4_clean(src)?;
            req = req.source_prefix(ip, 32);
        }

        req.execute().await?;
        Ok(())
    }

    pub async fn del_rule(&self, spec: &RuleSpec) -> Result<(), NetlinkError> {
        if spec.pref == 0 {
            return Err(NetlinkError::InvalidArgument(
                "rule priority must be non-zero for deletion".to_owned(),
            ));
        }

        let mut builder = self
            .handle
            .rule()
            .add()
            .v4()
            .action(RuleAction::ToTable)
            .priority(spec.pref);

        if spec.table_id > 255 {
            builder.message_mut().header.table = 0;
        }
        builder = builder.table_id(spec.table_id);

        if spec.fwmark > 0 {
            builder = builder.fw_mark(spec.fwmark);
            if spec.fwmask > 0 {
                builder
                    .message_mut()
                    .attributes
                    .push(RuleAttribute::FwMask(spec.fwmask));
            }
        }

        if let Some(src) = spec.src_ip.as_deref().filter(|s| !s.is_empty()) {
            let ip = parse_ipv4_clean(src)?;
            builder = builder.source_prefix(ip, 32);
        }

        let msg = builder.message_mut().clone();
        self.handle.rule().del(msg).execute().await?;
        Ok(())
    }

    pub async fn verify_rule(&self, table_id: Option<u32>, pref: u32) -> Result<bool, NetlinkError> {
        let mut rules = self.handle.rule().get(IpVersion::V4).execute();
        while let Some(rule) = rules.try_next().await? {
            let mut r_table = u32::from(rule.header.table);
            let mut r_pref = 0u32;
            for attr in &rule.attributes {
                match attr {
                    RuleAttribute::Table(t) => r_table = *t,
                    RuleAttribute::Priority(p) => r_pref = *p,
                    _ => {}
                }
            }
            if r_pref == pref && table_id.is_none_or(|t| r_table == t) {
                return Ok(true);
            }
        }
        Ok(false)
    }

    pub async fn add_default_route(
        &self,
        table_id: u32,
        gateway: &str,
        ifname: &str,
        metric: u32,
    ) -> Result<(), NetlinkError> {
        if table_id == 0 || gateway.is_empty() || ifname.is_empty() {
            return Err(NetlinkError::InvalidArgument(
                "invalid route parameters".to_owned(),
            ));
        }

        let ifindex = iface_index(ifname)?;
        let gw_ip = parse_ipv4_clean(gateway)?;

        let mut builder = RouteMessageBuilder::<Ipv4Addr>::new()
            .gateway(gw_ip)
            .output_interface(ifindex);
        if table_id > 255 {
            builder.get_mut().header.table = 0;
        }
        builder = builder.table_id(table_id);
        if metric > 0 {
            builder = builder.priority(metric);
        }

        self.handle.route().add(builder.build()).execute().await?;
        Ok(())
    }

    pub async fn del_default_route(
        &self,
        table_id: u32,
        gateway: &str,
        ifname: &str,
    ) -> Result<(), NetlinkError> {
        if table_id == 0 || gateway.is_empty() || ifname.is_empty() {
            return Err(NetlinkError::InvalidArgument(
                "invalid route parameters for deletion".to_owned(),
            ));
        }

        let ifindex = iface_index(ifname)?;
        let gw_ip = parse_ipv4_clean(gateway)?;

        let mut builder = RouteMessageBuilder::<Ipv4Addr>::new()
            .gateway(gw_ip)
            .output_interface(ifindex);
        if table_id > 255 {
            builder.get_mut().header.table = 0;
        }
        builder = builder.table_id(table_id);

        self.handle.route().del(builder.build()).execute().await?;
        Ok(())
    }

    pub async fn add_host_route(
        &self,
        table_id: u32,
        dst_ip: &str,
        gateway: &str,
        ifname: &str,
        metric: u32,
    ) -> Result<(), NetlinkError> {
        if table_id == 0 || dst_ip.is_empty() || gateway.is_empty() || ifname.is_empty() {
            return Err(NetlinkError::InvalidArgument(
                "invalid host route parameters".to_owned(),
            ));
        }

        let ifindex = iface_index(ifname)?;
        let dst = parse_ipv4_clean(dst_ip)?;
        let gw = parse_ipv4_clean(gateway)?;

        let mut builder = RouteMessageBuilder::<Ipv4Addr>::new()
            .destination_prefix(dst, 32)
            .gateway(gw)
            .output_interface(ifindex);
        if table_id > 255 {
            builder.get_mut().header.table = 0;
        }
        builder = builder.table_id(table_id);
        if metric > 0 {
            builder = builder.priority(metric);
        }

        self.handle
            .route()
            .add(builder.build())
            .replace()
            .execute()
            .await?;
        Ok(())
    }

    pub async fn del_host_route(
        &self,
        table_id: u32,
        dst_ip: &str,
        gateway: &str,
        ifname: &str,
    ) -> Result<(), NetlinkError> {
        if table_id == 0 || dst_ip.is_empty() || gateway.is_empty() || ifname.is_empty() {
            return Err(NetlinkError::InvalidArgument(
                "invalid host route parameters for deletion".to_owned(),
            ));
        }

        let ifindex = iface_index(ifname)?;
        let dst = parse_ipv4_clean(dst_ip)?;
        let gw = parse_ipv4_clean(gateway)?;

        let mut builder = RouteMessageBuilder::<Ipv4Addr>::new()
            .destination_prefix(dst, 32)
            .gateway(gw)
            .output_interface(ifindex);
        if table_id > 255 {
            builder.get_mut().header.table = 0;
        }
        builder = builder.table_id(table_id);

        self.handle.route().del(builder.build()).execute().await?;
        Ok(())
    }

    pub async fn verify_route(
        &self,
        table_id: u32,
        gateway: Option<&str>,
        ifname: Option<&str>,
    ) -> Result<bool, NetlinkError> {
        let target_oif = match ifname.filter(|s| !s.is_empty()) {
            Some(name) => Some(iface_index(name)?),
            None => None,
        };
        let target_gw = match gateway.filter(|s| !s.is_empty()) {
            Some(gw) => Some(parse_ipv4_clean(gw)?),
            None => None,
        };

        let mut query = RouteMessageBuilder::<Ipv4Addr>::new().build();
        query.header.table = 0;

        let mut routes = self.handle.route().get(query).execute();
        while let Some(route) = routes.try_next().await? {
            if route.header.address_family != netlink_packet_route::AddressFamily::Inet {
                continue;
            }
            let mut r_table = u32::from(route.header.table);
            let mut r_oif = None;
            let mut r_gw = None;

            for attr in &route.attributes {
                match attr {
                    netlink_packet_route::route::RouteAttribute::Table(t) => r_table = *t,
                    netlink_packet_route::route::RouteAttribute::Oif(o) => r_oif = Some(*o),
                    netlink_packet_route::route::RouteAttribute::Gateway(RouteAddress::Inet(ip)) => {
                        r_gw = Some(*ip);
                    }
                    _ => {}
                }
            }

            if r_table != table_id {
                continue;
            }
            let oif_ok = target_oif.is_none_or(|oif| r_oif == Some(oif));
            let gw_ok = target_gw.is_none_or(|gw| r_gw == Some(gw));
            if oif_ok && gw_ok {
                return Ok(true);
            }
        }
        Ok(false)
    }

    pub async fn table_has_routes(&self, table_id: u32) -> Result<bool, NetlinkError> {
        self.verify_route(table_id, None, None).await
    }
}

impl Drop for NetlinkSession {
    fn drop(&mut self) {
        self.driver.abort();
    }
}

pub fn iface_index(ifname: &str) -> Result<u32, NetlinkError> {
    if ifname.is_empty() || ifname.contains('\0') {
        return Err(NetlinkError::InvalidArgument(
            "empty or malformed interface name".to_owned(),
        ));
    }
    nix::net::if_::if_nametoindex(ifname)
        .map_err(|_| NetlinkError::InterfaceNotFound(ifname.to_owned()))
}

pub fn parse_ipv4_clean(raw: &str) -> Result<Ipv4Addr, NetlinkError> {
    let clean = raw.split('/').next().unwrap_or(raw).trim();
    clean.parse::<Ipv4Addr>().map_err(|_| {
        NetlinkError::InvalidArgument(format!("invalid IPv4 address '{raw}'"))
    })
}

#[cfg(all(test, target_os = "linux"))]
pub(crate) mod test_util {
    use super::{iface_index, NetlinkSession};
    use nix::sched::{unshare, CloneFlags};
    use nix::sys::wait::{waitpid, WaitStatus};
    use nix::unistd::{fork, ForkResult};
    use rtnetlink::LinkUnspec;
    use std::io::Write;
    use std::net::{IpAddr, Ipv4Addr};

    pub fn enter_isolated_netns() -> bool {
        let uid = nix::unistd::getuid();
        let gid = nix::unistd::getgid();
        if unshare(CloneFlags::CLONE_NEWUSER | CloneFlags::CLONE_NEWNET).is_err() {
            return false;
        }
        if let Ok(mut f) = std::fs::File::create("/proc/self/setgroups") {
            let _ = f.write_all(b"deny");
        }
        if let Ok(mut f) = std::fs::File::create("/proc/self/gid_map") {
            let _ = writeln!(f, "0 {gid} 1");
        }
        if let Ok(mut f) = std::fs::File::create("/proc/self/uid_map") {
            let _ = writeln!(f, "0 {uid} 1");
        }
        true
    }

    pub async fn setup_loopback(addrs: &[(Ipv4Addr, u8)]) {
        let nl = NetlinkSession::open().expect("open netlink for lo setup");
        let lo_idx = iface_index("lo").expect("lo index");
        nl.handle
            .link()
            .set(LinkUnspec::new_with_index(lo_idx).up().build())
            .execute()
            .await
            .expect("bring lo up");

        for &(ip, prefix) in addrs {
            nl.handle
                .address()
                .add(lo_idx, IpAddr::V4(ip), prefix)
                .execute()
                .await
                .expect("assign lo address");
        }
    }

    /// Runs `f` inside a forked `CLONE_NEWUSER | CLONE_NEWNET` child and propagates any panic as a test failure.
    pub fn run_in_isolated_netns<F>(f: F)
    where
        F: FnOnce() + std::panic::UnwindSafe,
    {
        // SAFETY: Fork precedes single-threaded Tokio runtime creation in the child; parent waits immediately.
        match unsafe { fork() } {
            Ok(ForkResult::Child) => {
                if !enter_isolated_netns() {
                    std::process::exit(0);
                }
                let result = std::panic::catch_unwind(f);
                std::process::exit(i32::from(result.is_err()));
            }
            Ok(ForkResult::Parent { child }) => {
                let status = waitpid(child, None).expect("waitpid");
                assert_eq!(status, WaitStatus::Exited(child, 0));
            }
            Err(e) => panic!("fork failed: {e}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clean_ipv4_strips_cidr() {
        assert_eq!(
            parse_ipv4_clean("192.168.1.9/24").expect("cidr"),
            Ipv4Addr::new(192, 168, 1, 9)
        );
        assert!(parse_ipv4_clean("not-an-ip").is_err());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn netns_rule_and_route_lifecycle() {
        test_util::run_in_isolated_netns(|| {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("rt");
            rt.block_on(async {
                test_util::setup_loopback(&[(Ipv4Addr::new(192, 168, 20, 2), 24)]).await;

                let nl = NetlinkSession::open().expect("open");
                let spec = RuleSpec {
                    table_id: 10002,
                    pref: 20002,
                    fwmark: 0,
                    fwmask: 0,
                    src_ip: Some("192.168.20.2/24".to_owned()),
                };
                nl.add_rule(&spec).await.expect("add rule");
                assert!(nl.verify_rule(Some(10002), 20002).await.expect("verify"));

                nl.add_default_route(10002, "192.168.20.1", "lo", 100)
                    .await
                    .expect("add route");
                assert!(nl
                    .verify_route(10002, Some("192.168.20.1"), Some("lo"))
                    .await
                    .expect("verify route"));

                nl.del_default_route(10002, "192.168.20.1", "lo")
                    .await
                    .expect("del route");
                assert!(!nl
                    .verify_route(10002, Some("192.168.20.1"), Some("lo"))
                    .await
                    .expect("verify absent"));

                nl.del_rule(&spec).await.expect("del rule");
                assert!(!nl.verify_rule(Some(10002), 20002).await.expect("absent"));
            });
        });
    }
}
