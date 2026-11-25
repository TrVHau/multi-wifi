#![deny(unsafe_code)]

use nix::sys::utsname::uname;
use socket2::{Domain, Protocol, Socket, Type};
use std::fs;
use std::path::Path;
use std::time::Duration;

const IPPROTO_MPTCP: i32 = 262;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CapState {
    #[default]
    Unknown = 0,
    Unsupported,
    AvailableInactive,
    Active,
}

impl CapState {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Unsupported => "Unsupported",
            Self::AvailableInactive => "Available (Inactive)",
            Self::Active => "Active",
            Self::Unknown => "Unknown",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Capabilities {
    pub kernel_mptcp: CapState,
    pub nftables: CapState,
    pub tun_device: CapState,
    pub network_manager: CapState,
    pub mptcp_details: String,
    pub kernel_version: String,
}

impl Capabilities {
    /// Probes host kernel and platform capabilities synchronously without blocking forever.
    #[must_use]
    pub fn probe_sync() -> Self {
        let mut caps = Self::probe_local_subsystems();
        caps.network_manager = probe_network_manager_fs_hint();
        caps
    }

    /// Probes host kernel capabilities plus live `NetworkManager` D-Bus ownership asynchronously.
    pub async fn probe_async() -> Self {
        let mut caps = Self::probe_local_subsystems();
        caps.network_manager = probe_network_manager_dbus().await;
        caps
    }

    fn probe_local_subsystems() -> Self {
        let kernel_version = uname().map_or_else(
            |_| "Unknown Kernel".to_owned(),
            |u| {
                format!(
                    "{} {}",
                    u.sysname().to_string_lossy(),
                    u.release().to_string_lossy()
                )
            },
        );

        let tun_device = if Path::new("/dev/net/tun").exists() {
            CapState::AvailableInactive
        } else {
            CapState::Unsupported
        };

        let nftables = if is_executable(Path::new("/usr/bin/nft"))
            || is_executable(Path::new("/usr/sbin/nft"))
            || Path::new("/proc/sys/net/netfilter").is_dir()
        {
            CapState::AvailableInactive
        } else {
            CapState::Unsupported
        };

        let sysctl_val = fs::read_to_string("/proc/sys/net/mptcp/enabled")
            .ok()
            .map(|s| s.trim().to_owned());

        let (kernel_mptcp, mptcp_details) =
            match Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::from(IPPROTO_MPTCP))) {
                Ok(_sock) => (
                    CapState::AvailableInactive,
                    format!(
                        "Kernel MPTCP socket supported (sysctl enabled={})",
                        sysctl_val.as_deref().unwrap_or("unknown")
                    ),
                ),
                Err(err) => (
                    CapState::Unsupported,
                    format!(
                        "IPPROTO_MPTCP socket failed: {err} ({})",
                        err.raw_os_error().unwrap_or(-1)
                    ),
                ),
            };

        Self {
            kernel_mptcp,
            nftables,
            tun_device,
            network_manager: CapState::Unknown,
            mptcp_details,
            kernel_version,
        }
    }
}

fn is_executable(path: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::metadata(path)
            .is_ok_and(|m| m.is_file() && (m.permissions().mode() & 0o111 != 0))
    }
    #[cfg(not(unix))]
    {
        path.is_file()
    }
}

fn probe_network_manager_fs_hint() -> CapState {
    if Path::new("/run/NetworkManager").is_dir() {
        CapState::Active
    } else {
        CapState::AvailableInactive
    }
}

async fn probe_network_manager_dbus() -> CapState {
    let probe_fut = async {
        let conn = zbus::Connection::system().await.ok()?;
        let dbus = zbus::fdo::DBusProxy::new(&conn).await.ok()?;
        let nm_name = zbus::names::BusName::try_from("org.freedesktop.NetworkManager").ok()?;
        let has_owner = dbus.name_has_owner(nm_name).await.ok()?;
        Some(if has_owner {
            CapState::Active
        } else {
            CapState::AvailableInactive
        })
    };

    if let Ok(Some(state)) = tokio::time::timeout(Duration::from_millis(250), probe_fut).await {
        return state;
    }

    probe_network_manager_fs_hint()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn probe_sync_does_not_panic_and_reports_kernel() {
        let caps = Capabilities::probe_sync();
        assert!(!caps.kernel_version.is_empty());
        assert!(!caps.mptcp_details.is_empty());
        assert_ne!(caps.network_manager, CapState::Unknown);
    }

    #[tokio::test]
    async fn probe_async_completes_within_budget() {
        let caps = Capabilities::probe_async().await;
        assert!(!caps.kernel_version.is_empty());
        assert_ne!(caps.network_manager, CapState::Unknown);
    }
}
