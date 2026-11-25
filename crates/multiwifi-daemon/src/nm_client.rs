#![deny(unsafe_code)]

use crate::operation::insert_val;
use futures_util::stream::StreamExt;
use multiwifi_core::netlink_route::iface_index;
use multiwifi_core::path_monitor::{DevicePathInput, PathMonitor};
use nix::ifaddrs::getifaddrs;
use std::collections::HashMap;
use std::fs;
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;
use tokio::sync::watch;
use tokio::task::JoinHandle;
use zbus::message::Type as MessageType;
use zbus::{Connection, MatchRule, MessageStream, OwnedObjectPath, OwnedVariant, Proxy};

const NM_SERVICE: &str = "org.freedesktop.NetworkManager";
const NM_PATH: &str = "/org/freedesktop/NetworkManager";
const NM_IFACE: &str = "org.freedesktop.NetworkManager";
const NM_DEVICE_IFACE: &str = "org.freedesktop.NetworkManager.Device";
const NM_WIRELESS_IFACE: &str = "org.freedesktop.NetworkManager.Device.Wireless";
const NM_AP_IFACE: &str = "org.freedesktop.NetworkManager.AccessPoint";
const NM_IP4_IFACE: &str = "org.freedesktop.NetworkManager.IP4Config";
const NM_IP6_IFACE: &str = "org.freedesktop.NetworkManager.IP6Config";
const NM_ACTIVE_IFACE: &str = "org.freedesktop.NetworkManager.Connection.Active";

const NM_DEVICE_TYPE_WIFI: u32 = 2;
const NM_QUERY_TIMEOUT: Duration = Duration::from_millis(1200);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum WifiDeviceState {
    #[default]
    Unknown,
    Unavailable,
    Disconnected,
    Preparing,
    Configuring,
    NeedAuth,
    IpConfig,
    Connected,
    Failed,
}

impl WifiDeviceState {
    #[must_use]
    pub const fn from_nm(nm_state: u32) -> Self {
        match nm_state {
            10 | 20 => Self::Unavailable,
            30 | 110 => Self::Disconnected,
            40 => Self::Preparing,
            50 => Self::Configuring,
            60 => Self::NeedAuth,
            70..=90 => Self::IpConfig,
            100 => Self::Connected,
            120 => Self::Failed,
            _ => Self::Unknown,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Connected => "Connected",
            Self::Disconnected => "Disconnected",
            Self::Preparing => "Preparing",
            Self::Configuring => "Configuring",
            Self::NeedAuth => "Needs Authentication",
            Self::IpConfig => "Obtaining IP",
            Self::Failed => "Failed",
            Self::Unavailable => "Unavailable",
            Self::Unknown => "Unknown",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WifiDeviceInfo {
    pub ifname: String,
    pub ifindex: i32,
    pub hw_address: String,
    pub state: WifiDeviceState,
    pub state_str: String,
    pub ssid: String,
    pub bssid: String,
    pub signal_strength: u8,
    pub frequency: u32,
    pub ipv4_address: String,
    pub ipv4_gateway: String,
    pub ipv6_address: String,
    pub connection_uuid: String,
    pub connection_id: String,
    pub managed: bool,
}

impl WifiDeviceInfo {
    #[must_use]
    pub fn to_dbus_dict(&self) -> HashMap<String, OwnedVariant> {
        let mut map = HashMap::new();
        insert_val(&mut map, "ifname", self.ifname.clone());
        insert_val(&mut map, "ifindex", self.ifindex);
        insert_val(&mut map, "hw_address", self.hw_address.clone());
        insert_val(&mut map, "state", self.state_str.clone());
        insert_val(&mut map, "ssid", self.ssid.clone());
        insert_val(&mut map, "bssid", self.bssid.clone());
        insert_val(&mut map, "signal_strength", self.signal_strength);
        insert_val(&mut map, "frequency", self.frequency);
        insert_val(&mut map, "ipv4_address", self.ipv4_address.clone());
        insert_val(&mut map, "ipv4_gateway", self.ipv4_gateway.clone());
        insert_val(&mut map, "ipv6_address", self.ipv6_address.clone());
        insert_val(&mut map, "connection_uuid", self.connection_uuid.clone());
        insert_val(&mut map, "connection_id", self.connection_id.clone());
        insert_val(&mut map, "managed", self.managed);
        map
    }
}

/// Queries kernel `getifaddrs` and `/proc/net/route` as an authoritative fallback for IPv4/gateway.
pub fn fill_kernel_ipv4(ifname: &str, ipv4_address: &mut String, ipv4_gateway: &mut String) {
    if ifname.is_empty() {
        return;
    }

    if ipv4_address.is_empty() {
        if let Ok(addrs) = getifaddrs() {
            for ifa in addrs {
                if ifa.interface_name != ifname {
                    continue;
                }
                let Some(sin) = ifa.address.and_then(|s| s.as_sockaddr_in().copied()) else {
                    continue;
                };
                let ip = sin.ip();
                let mut prefix = ifa
                    .netmask
                    .and_then(|s| s.as_sockaddr_in().map(|m| u32::from(m.ip()).leading_ones()))
                    .unwrap_or(0);
                if prefix == 0 {
                    prefix = 24;
                }
                *ipv4_address = format!("{ip}/{prefix}");
                break;
            }
        }
    }

    if ipv4_gateway.is_empty() {
        if let Ok(route_table) = fs::read_to_string("/proc/net/route") {
            if let Some(gw) = parse_default_gw_from_proc_route(ifname, &route_table) {
                *ipv4_gateway = gw.to_string();
            }
        }
    }
}

fn parse_default_gw_from_proc_route(ifname: &str, content: &str) -> Option<Ipv4Addr> {
    for line in content.lines().skip(1) {
        let mut cols = line.split_whitespace();
        let (Some(dev), Some(dest_hex), Some(gw_hex), Some(flags_hex)) =
            (cols.next(), cols.next(), cols.next(), cols.next())
        else {
            continue;
        };
        if dev != ifname {
            continue;
        }
        let Ok(dest) = u32::from_str_radix(dest_hex, 16) else {
            continue;
        };
        let Ok(gw_raw) = u32::from_str_radix(gw_hex, 16) else {
            continue;
        };
        let Ok(flags) = u32::from_str_radix(flags_hex, 16) else {
            continue;
        };
        if dest == 0 && (flags & 0x2 != 0) {
            return Some(Ipv4Addr::from(gw_raw.to_ne_bytes()));
        }
    }
    None
}

struct InventoryInner {
    cached: Mutex<Vec<WifiDeviceInfo>>,
    tx: watch::Sender<Vec<WifiDeviceInfo>>,
    watcher: Mutex<Option<JoinHandle<()>>>,
}

/// Async D-Bus `NetworkManager` Wi-Fi inventory tracker feeding `PathMonitor` and `DeviceChanged` events.
#[derive(Clone)]
pub struct NmInventory {
    inner: Arc<InventoryInner>,
}

impl Default for NmInventory {
    fn default() -> Self {
        Self::new()
    }
}

impl NmInventory {
    #[must_use]
    pub fn new() -> Self {
        let (tx, _) = watch::channel(Vec::new());
        Self {
            inner: Arc::new(InventoryInner {
                cached: Mutex::new(Vec::new()),
                tx,
                watcher: Mutex::new(None),
            }),
        }
    }

    #[must_use]
    pub fn subscribe(&self) -> watch::Receiver<Vec<WifiDeviceInfo>> {
        self.inner.tx.subscribe()
    }

    #[must_use]
    pub fn devices(&self) -> Vec<WifiDeviceInfo> {
        self.inner
            .cached
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone()
    }

    pub async fn refresh(&self) -> Vec<WifiDeviceInfo> {
        let queried = tokio::time::timeout(NM_QUERY_TIMEOUT, query_nm_wifi_devices())
            .await
            .ok()
            .and_then(Result::ok)
            .unwrap_or_default();

        {
            let mut cached = self
                .inner
                .cached
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            cached.clone_from(&queried);
        }
        let _ = self.inner.tx.send(queried.clone());
        queried
    }

    /// Starts background D-Bus listening for `NetworkManager` topology and state transitions.
    pub fn start(&self, path_monitor: PathMonitor, is_system_bus: bool) {
        let mut guard = self
            .inner
            .watcher
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        if guard.is_some() {
            return;
        }
        let inv = self.clone();
        let handle = tokio::spawn(async move {
            if !is_system_bus {
                return;
            }
            let initial = inv.refresh().await;
            sync_devices_to_monitor(&path_monitor, &initial);

            let Ok(sys_conn) = Connection::system().await else {
                return;
            };
            let Ok(rule) = MatchRule::builder()
                .msg_type(MessageType::Signal)
                .sender(NM_SERVICE)
                .build()
            else {
                return;
            };
            let Ok(dbus_proxy) = zbus::fdo::DBusProxy::new(&sys_conn).await else {
                return;
            };
            let _ = dbus_proxy.add_match_rule(rule).await;

            let mut stream = MessageStream::from(&sys_conn);
            while let Some(msg) = stream.next().await {
                let Ok(msg) = msg else {
                    break;
                };
                let hdr = msg.header();
                if hdr.message_type() != MessageType::Signal {
                    continue;
                }
                let member = hdr.member().map(zbus::names::MemberName::as_str);
                if matches!(
                    member,
                    Some("DeviceAdded" | "DeviceRemoved" | "StateChanged" | "PropertiesChanged")
                ) {
                    let updated = inv.refresh().await;
                    sync_devices_to_monitor(&path_monitor, &updated);
                }
            }
        });
        *guard = Some(handle);
    }

    pub fn stop(&self) {
        let mut guard = self
            .inner
            .watcher
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        if let Some(handle) = guard.take() {
            handle.abort();
        }
    }
}

impl Drop for NmInventory {
    fn drop(&mut self) {
        if Arc::strong_count(&self.inner) == 1 {
            self.stop();
        }
    }
}

pub fn sync_devices_to_monitor(monitor: &PathMonitor, devices: &[WifiDeviceInfo]) {
    for d in devices {
        monitor.update_device(DevicePathInput {
            ifname: &d.ifname,
            connected: d.state == WifiDeviceState::Connected,
            status_reason: &d.state_str,
            ipv4_address: (!d.ipv4_address.is_empty()).then_some(d.ipv4_address.as_str()),
            ipv4_gateway: (!d.ipv4_gateway.is_empty()).then_some(d.ipv4_gateway.as_str()),
        });
    }
}

async fn query_nm_wifi_devices() -> zbus::Result<Vec<WifiDeviceInfo>> {
    let conn = Connection::system().await?;
    let nm_proxy = Proxy::new(&conn, NM_SERVICE, NM_PATH, NM_IFACE).await?;
    let dev_paths: Vec<OwnedObjectPath> = nm_proxy.call("GetDevices", &()).await?;

    let mut out = Vec::new();
    for dev_path in dev_paths {
        if let Ok(Some(info)) = inspect_nm_device(&conn, &dev_path).await {
            out.push(info);
        }
    }
    Ok(out)
}

async fn inspect_nm_device(
    conn: &Connection,
    dev_path: &OwnedObjectPath,
) -> zbus::Result<Option<WifiDeviceInfo>> {
    let dev = Proxy::new(conn, NM_SERVICE, dev_path.as_ref(), NM_DEVICE_IFACE).await?;
    let dev_type: u32 = dev.get_property("DeviceType").await?;
    if dev_type != NM_DEVICE_TYPE_WIFI {
        return Ok(None);
    }

    let ifname: String = dev.get_property("Interface").await.unwrap_or_default();
    let managed: bool = dev.get_property("Managed").await.unwrap_or(true);
    let nm_state: u32 = dev.get_property("State").await.unwrap_or(0);
    let state = WifiDeviceState::from_nm(nm_state);
    let state_str = state.as_str().to_owned();
    let ifindex = iface_index(&ifname)
        .ok()
        .and_then(|idx| i32::try_from(idx).ok())
        .unwrap_or(-1);

    let mut hw_address: String = dev.get_property("HwAddress").await.unwrap_or_default();
    let wireless = Proxy::new(conn, NM_SERVICE, dev_path.as_ref(), NM_WIRELESS_IFACE).await?;
    if hw_address.is_empty() {
        hw_address = wireless
            .get_property("PermHwAddress")
            .await
            .unwrap_or_default();
    }

    let (ssid, bssid, signal_strength, frequency) = read_active_ap(conn, &wireless).await;
    let (mut ipv4_address, mut ipv4_gateway) = read_ip4_config(conn, &dev).await;
    let ipv6_address = read_ip6_config(conn, &dev).await;
    let (connection_uuid, connection_id) = read_active_connection(conn, &dev).await;

    if state == WifiDeviceState::Connected && (ipv4_address.is_empty() || ipv4_gateway.is_empty()) {
        fill_kernel_ipv4(&ifname, &mut ipv4_address, &mut ipv4_gateway);
    }

    Ok(Some(WifiDeviceInfo {
        ifname,
        ifindex,
        hw_address,
        state,
        state_str,
        ssid,
        bssid,
        signal_strength,
        frequency,
        ipv4_address,
        ipv4_gateway,
        ipv6_address,
        connection_uuid,
        connection_id,
        managed,
    }))
}

async fn read_active_ap(conn: &Connection, wireless: &Proxy<'_>) -> (String, String, u8, u32) {
    let Ok(ap_path) = wireless
        .get_property::<OwnedObjectPath>("ActiveAccessPoint")
        .await
    else {
        return (String::new(), String::new(), 0, 0);
    };
    if ap_path.as_str() == "/" {
        return (String::new(), String::new(), 0, 0);
    }
    let Ok(ap) = Proxy::new(conn, NM_SERVICE, ap_path.as_ref(), NM_AP_IFACE).await else {
        return (String::new(), String::new(), 0, 0);
    };
    let ssid_bytes: Vec<u8> = ap.get_property("Ssid").await.unwrap_or_default();
    let ssid = String::from_utf8_lossy(&ssid_bytes).into_owned();
    let bssid: String = ap.get_property("HwAddress").await.unwrap_or_default();
    let strength: u8 = ap.get_property("Strength").await.unwrap_or(0);
    let freq: u32 = ap.get_property("Frequency").await.unwrap_or(0);
    (ssid, bssid, strength, freq)
}

async fn read_ip4_config(conn: &Connection, dev: &Proxy<'_>) -> (String, String) {
    let Ok(ip4_path) = dev.get_property::<OwnedObjectPath>("Ip4Config").await else {
        return (String::new(), String::new());
    };
    if ip4_path.as_str() == "/" {
        return (String::new(), String::new());
    }
    let Ok(ip4) = Proxy::new(conn, NM_SERVICE, ip4_path.as_ref(), NM_IP4_IFACE).await else {
        return (String::new(), String::new());
    };
    let gw: String = ip4.get_property("Gateway").await.unwrap_or_default();
    let addr = read_first_address_data(&ip4).await;
    (addr, gw)
}

async fn read_ip6_config(conn: &Connection, dev: &Proxy<'_>) -> String {
    let Ok(ip6_path) = dev.get_property::<OwnedObjectPath>("Ip6Config").await else {
        return String::new();
    };
    if ip6_path.as_str() == "/" {
        return String::new();
    }
    let Ok(ip6) = Proxy::new(conn, NM_SERVICE, ip6_path.as_ref(), NM_IP6_IFACE).await else {
        return String::new();
    };
    read_first_address_data(&ip6).await
}

async fn read_first_address_data(ip_cfg: &Proxy<'_>) -> String {
    let Ok(list) = ip_cfg
        .get_property::<Vec<HashMap<String, OwnedVariant>>>("AddressData")
        .await
    else {
        return String::new();
    };
    let Some(first) = list.first() else {
        return String::new();
    };
    let addr = first
        .get("address")
        .and_then(|v| <&str>::try_from(v).ok())
        .unwrap_or_default();
    if addr.is_empty() {
        return String::new();
    }
    let prefix = first
        .get("prefix")
        .and_then(|v| u32::try_from(v).ok())
        .unwrap_or(0);
    format!("{addr}/{prefix}")
}

async fn read_active_connection(conn: &Connection, dev: &Proxy<'_>) -> (String, String) {
    let Ok(ac_path) = dev
        .get_property::<OwnedObjectPath>("ActiveConnection")
        .await
    else {
        return (String::new(), String::new());
    };
    if ac_path.as_str() == "/" {
        return (String::new(), String::new());
    }
    let Ok(ac) = Proxy::new(conn, NM_SERVICE, ac_path.as_ref(), NM_ACTIVE_IFACE).await else {
        return (String::new(), String::new());
    };
    let uuid: String = ac.get_property("Uuid").await.unwrap_or_default();
    let id: String = ac.get_property("Id").await.unwrap_or_default();
    (uuid, id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nm_device_state_mapping() {
        assert_eq!(WifiDeviceState::from_nm(100), WifiDeviceState::Connected);
        assert_eq!(WifiDeviceState::from_nm(30), WifiDeviceState::Disconnected);
        assert_eq!(WifiDeviceState::from_nm(60), WifiDeviceState::NeedAuth);
        assert_eq!(WifiDeviceState::from_nm(120), WifiDeviceState::Failed);
        assert_eq!(WifiDeviceState::Connected.as_str(), "Connected");
    }

    #[test]
    fn proc_net_route_parser_handles_le_gateway_hex() {
        let sample = "\
Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT
wlp0s20f3\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0
wlp0s20f3\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0
";
        let expected = Ipv4Addr::from(0x0101_A8C0u32.to_ne_bytes());
        assert_eq!(
            parse_default_gw_from_proc_route("wlp0s20f3", sample),
            Some(expected)
        );
        assert_eq!(parse_default_gw_from_proc_route("wlan1", sample), None);
    }
}
