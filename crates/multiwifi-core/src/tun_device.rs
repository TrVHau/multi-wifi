use crate::netlink_route::parse_ipv4_clean;
use nix::fcntl::{fcntl, FcntlArg, OFlag};
use nix::sys::socket::{socket, AddressFamily, SockFlag, SockType};
use nix::unistd::{read, write};
use std::fs::OpenOptions;
use std::io;
use std::net::Ipv4Addr;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd, RawFd};
use std::os::unix::fs::OpenOptionsExt;
use thiserror::Error;
use tokio::io::unix::AsyncFd;

pub const DEFAULT_TUN_NAME: &str = "mwtun0";
pub const DEFAULT_TUN_MTU: u32 = 1420;

#[derive(Debug, Error)]
pub enum TunError {
    #[error("invalid TUN configuration: {0}")]
    InvalidArgument(String),

    #[error("TUN device is not open")]
    NotOpen,

    #[error("TUN device I/O error: {0}")]
    Io(#[from] io::Error),
}

mod ioctl_ffi {
    use super::{io, Ipv4Addr};
    use std::ffi::CStr;
    use std::os::fd::RawFd;

    const IFF_TUN: libc::c_short = 0x0001;
    const IFF_NO_PI: libc::c_short = 0x1000;
    const SIOCGIFFLAGS: libc::Ioctl = 0x8913;
    const SIOCSIFFLAGS: libc::Ioctl = 0x8914;
    const SIOCSIFADDR: libc::Ioctl = 0x8916;
    const SIOCSIFNETMASK: libc::Ioctl = 0x891c;
    const SIOCSIFMTU: libc::Ioctl = 0x8922;

    fn tun_set_iff_req() -> libc::Ioctl {
        nix::request_code_write!(b'T', 202, std::mem::size_of::<libc::c_int>()) as libc::Ioctl
    }

    fn init_ifreq(ifname: &str) -> Result<libc::ifreq, io::Error> {
        let bytes = ifname.as_bytes();
        if bytes.is_empty() || bytes.len() >= libc::IFNAMSIZ || bytes.contains(&0) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("invalid interface name '{ifname}'"),
            ));
        }

        // SAFETY: `libc::ifreq` is a C POD struct/union; all-zero initialization is valid.
        let mut ifr: libc::ifreq = unsafe { std::mem::zeroed() };
        // SAFETY: `bytes.len() < libc::IFNAMSIZ` is checked above and dst is zeroed.
        unsafe {
            std::ptr::copy_nonoverlapping(
                bytes.as_ptr().cast::<libc::c_char>(),
                ifr.ifr_name.as_mut_ptr(),
                bytes.len(),
            );
        }
        Ok(ifr)
    }

    fn set_ifreq_ipv4(dst: &mut libc::sockaddr, ip: Ipv4Addr) {
        const {
            assert!(
                std::mem::size_of::<libc::sockaddr_in>() == std::mem::size_of::<libc::sockaddr>()
            );
        }
        let sin = libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: 0,
            sin_addr: libc::in_addr {
                s_addr: u32::from(ip).to_be(),
            },
            sin_zero: [0; 8],
        };
        // SAFETY: Both `sockaddr_in` and `sockaddr` have identical 16-byte size and represent an IPv4 socket address.
        unsafe {
            std::ptr::copy_nonoverlapping(
                (&raw const sin).cast::<u8>(),
                (&raw mut *dst).cast::<u8>(),
                std::mem::size_of::<libc::sockaddr_in>(),
            );
        }
    }

    pub fn tun_set_iff(fd: RawFd, desired_name: &str) -> io::Result<String> {
        let mut ifr = init_ifreq(desired_name)?;
        ifr.ifr_ifru.ifru_flags = IFF_TUN | IFF_NO_PI;

        // SAFETY: `fd` is an open `/dev/net/tun` file descriptor and `ifr` is a valid initialized `libc::ifreq`.
        let rc = unsafe { libc::ioctl(fd, tun_set_iff_req(), &raw mut ifr) };
        if rc < 0 {
            return Err(io::Error::last_os_error());
        }

        // Ensure nul-termination before inspecting returned kernel interface name.
        ifr.ifr_name[libc::IFNAMSIZ - 1] = 0;
        // SAFETY: `ifr.ifr_name` is guaranteed nul-terminated and lives for the duration of the borrow.
        let actual = unsafe { CStr::from_ptr(ifr.ifr_name.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Ok(actual)
    }

    pub fn set_mtu(sock_fd: RawFd, ifname: &str, mtu: u32) -> io::Result<()> {
        let mut ifr = init_ifreq(ifname)?;
        ifr.ifr_ifru.ifru_mtu = i32::try_from(mtu)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "MTU exceeds i32::MAX"))?;

        // SAFETY: `sock_fd` is a valid AF_INET DGRAM socket and `ifr` carries a valid `ifru_mtu`.
        let rc = unsafe { libc::ioctl(sock_fd, SIOCSIFMTU, &raw mut ifr) };
        if rc < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    pub fn configure_ipv4_and_up(
        sock_fd: RawFd,
        ifname: &str,
        ipv4: Ipv4Addr,
        netmask: Ipv4Addr,
    ) -> io::Result<()> {
        let mut ifr = init_ifreq(ifname)?;
        // SAFETY: Accessing `ifru_addr` to write the IPv4 `sockaddr_in` for `SIOCSIFADDR`.
        unsafe { set_ifreq_ipv4(&mut ifr.ifr_ifru.ifru_addr, ipv4) };
        // SAFETY: `sock_fd` is a valid AF_INET socket and `ifr` holds the IPv4 address.
        if unsafe { libc::ioctl(sock_fd, SIOCSIFADDR, &raw mut ifr) } < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut mask_ifr = init_ifreq(ifname)?;
        // SAFETY: Accessing `ifru_netmask` to write the subnet mask `sockaddr_in` for `SIOCSIFNETMASK`.
        unsafe { set_ifreq_ipv4(&mut mask_ifr.ifr_ifru.ifru_netmask, netmask) };
        // SAFETY: `sock_fd` is a valid AF_INET socket and `mask_ifr` holds the IPv4 netmask.
        if unsafe { libc::ioctl(sock_fd, SIOCSIFNETMASK, &raw mut mask_ifr) } < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut flag_ifr = init_ifreq(ifname)?;
        // SAFETY: `SIOCGIFFLAGS` populates `flag_ifr.ifr_ifru.ifru_flags`.
        if unsafe { libc::ioctl(sock_fd, SIOCGIFFLAGS, &raw mut flag_ifr) } < 0 {
            return Err(io::Error::last_os_error());
        }
        let up_flags = i16::try_from(libc::IFF_UP | libc::IFF_RUNNING)
            .map_err(|_| io::Error::other("IFF_UP | IFF_RUNNING out of range"))?;
        // SAFETY: Reading then updating `ifru_flags` after successful `SIOCGIFFLAGS`.
        unsafe {
            flag_ifr.ifr_ifru.ifru_flags |= up_flags;
        }
        // SAFETY: `sock_fd` is a valid AF_INET socket and `flag_ifr` carries updated `ifru_flags`.
        if unsafe { libc::ioctl(sock_fd, SIOCSIFFLAGS, &raw mut flag_ifr) } < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }
}

/// Manages a Linux `/dev/net/tun` Layer-3 interface (or an injectable mock fd for unit tests).
#[derive(Debug)]
pub struct TunDevice {
    desired_name: String,
    actual_name: String,
    mtu: u32,
    fd: Option<OwnedFd>,
    is_mock: bool,
}

impl TunDevice {
    #[must_use]
    pub fn new(name: Option<&str>, mtu: u32) -> Self {
        let resolved_name = name
            .filter(|s| !s.is_empty())
            .unwrap_or(DEFAULT_TUN_NAME)
            .to_owned();
        let resolved_mtu = if mtu == 0 { DEFAULT_TUN_MTU } else { mtu };
        Self {
            desired_name: resolved_name.clone(),
            actual_name: resolved_name,
            mtu: resolved_mtu,
            fd: None,
            is_mock: false,
        }
    }

    pub fn from_mock_fd(name: Option<&str>, mtu: u32, fd: OwnedFd) -> Result<Self, TunError> {
        let mut dev = Self::new(name, mtu);
        dev.set_mock_fd(fd)?;
        Ok(dev)
    }

    pub fn set_mock_fd(&mut self, fd: OwnedFd) -> Result<(), TunError> {
        set_nonblocking(fd.as_fd())?;
        self.fd = Some(fd);
        self.is_mock = true;
        self.actual_name.clone_from(&self.desired_name);
        Ok(())
    }

    pub fn open(&mut self) -> Result<(), TunError> {
        if self.fd.is_some() {
            return Ok(());
        }

        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
            .open("/dev/net/tun")?;
        let owned: OwnedFd = file.into();
        let actual = ioctl_ffi::tun_set_iff(owned.as_raw_fd(), &self.desired_name)?;

        self.actual_name = actual;
        self.is_mock = false;
        self.fd = Some(owned);

        if self.mtu > 0 {
            let sock = socket(
                AddressFamily::Inet,
                SockType::Datagram,
                SockFlag::SOCK_CLOEXEC,
                None,
            )
            .map_err(|e| io::Error::from_raw_os_error(e as i32))?;
            let _ = ioctl_ffi::set_mtu(sock.as_raw_fd(), &self.actual_name, self.mtu);
        }

        Ok(())
    }

    pub fn close(&mut self) {
        self.fd = None;
        self.is_mock = false;
    }

    #[must_use]
    pub const fn is_open(&self) -> bool {
        self.fd.is_some()
    }

    #[must_use]
    pub fn raw_fd(&self) -> Option<RawFd> {
        self.fd.as_ref().map(AsRawFd::as_raw_fd)
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.actual_name
    }

    #[must_use]
    pub const fn mtu(&self) -> u32 {
        self.mtu
    }

    pub fn configure_ip(&self, ipv4_addr: &str, netmask: &str) -> Result<(), TunError> {
        if self.fd.is_none() {
            return Err(TunError::NotOpen);
        }
        let ipv4 =
            parse_ipv4_clean(ipv4_addr).map_err(|e| TunError::InvalidArgument(e.to_string()))?;
        let mask: Ipv4Addr = netmask
            .trim()
            .parse()
            .map_err(|_| TunError::InvalidArgument(format!("invalid IPv4 netmask '{netmask}'")))?;

        if self.is_mock {
            return Ok(());
        }

        let sock = socket(
            AddressFamily::Inet,
            SockType::Datagram,
            SockFlag::SOCK_CLOEXEC,
            None,
        )
        .map_err(|e| io::Error::from_raw_os_error(e as i32))?;
        ioctl_ffi::configure_ipv4_and_up(sock.as_raw_fd(), &self.actual_name, ipv4, mask)?;
        Ok(())
    }

    pub fn read_nonblocking(&self, buf: &mut [u8]) -> io::Result<usize> {
        let fd = self
            .fd
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "TUN device not open"))?;
        read(fd, buf).map_err(|e| io::Error::from_raw_os_error(e as i32))
    }

    pub fn write_nonblocking(&self, buf: &[u8]) -> io::Result<usize> {
        let fd = self
            .fd
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "TUN device not open"))?;
        write(fd, buf).map_err(|e| io::Error::from_raw_os_error(e as i32))
    }

    /// Clones the underlying non-blocking descriptor into an async-ready `AsyncFd`.
    pub fn clone_async_fd(&self) -> io::Result<AsyncFd<OwnedFd>> {
        let fd = self
            .fd
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "TUN device not open"))?;
        let dup = fd.try_clone()?;
        register_owned_async_fd(dup)
    }
}

/// Registers an owned descriptor with Tokio's reactor in non-blocking mode.
pub fn register_owned_async_fd(fd: OwnedFd) -> io::Result<AsyncFd<OwnedFd>> {
    set_nonblocking(fd.as_fd())?;
    // SAFETY: `OwnedFd` uniquely owns an open kernel descriptor that stays open and constant until `AsyncFd` drops.
    let async_fd = unsafe { AsyncFd::register(fd)? };
    Ok(async_fd)
}

fn set_nonblocking(fd: BorrowedFd<'_>) -> io::Result<()> {
    let raw_flags =
        fcntl(fd, FcntlArg::F_GETFL).map_err(|e| io::Error::from_raw_os_error(e as i32))?;
    let mut oflags = OFlag::from_bits_retain(raw_flags);
    oflags.insert(OFlag::O_NONBLOCK | OFlag::O_CLOEXEC);
    fcntl(fd, FcntlArg::F_SETFL(oflags)).map_err(|e| io::Error::from_raw_os_error(e as i32))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use nix::sys::socket::{socketpair, AddressFamily, SockFlag, SockType};
    use std::io::ErrorKind;

    #[test]
    fn mock_tun_lifecycle_and_read_write() {
        let (sv0, sv1) = socketpair(
            AddressFamily::Unix,
            SockType::Stream,
            None,
            SockFlag::SOCK_CLOEXEC,
        )
        .expect("socketpair");

        let mut tun = TunDevice::from_mock_fd(Some("mwtun9"), 1420, sv0).expect("mock tun");
        assert!(tun.is_open());
        assert_eq!(tun.name(), "mwtun9");
        assert_eq!(tun.mtu(), 1420);

        tun.configure_ip("10.8.0.2", "255.255.255.0")
            .expect("mock configure ip");
        assert!(tun.configure_ip("not-an-ip", "255.255.255.0").is_err());

        let mut empty_buf = [0u8; 64];
        let err = tun
            .read_nonblocking(&mut empty_buf)
            .expect_err("would block");
        assert_eq!(err.kind(), ErrorKind::WouldBlock);

        let pkt = [0x45, 0x00, 0x00, 0x14, 0x01, 0x02, 0x03, 0x04];
        assert_eq!(tun.write_nonblocking(&pkt).expect("write"), pkt.len());

        let mut peer_buf = [0u8; 64];
        let n_peer = read(&sv1, &mut peer_buf).expect("peer read");
        assert_eq!(&peer_buf[..n_peer], &pkt);

        let reply = [0x45, 0x00, 0x00, 0x14, 0x05, 0x06, 0x07, 0x08];
        assert_eq!(write(&sv1, &reply).expect("peer write"), reply.len());

        let n_tun = tun.read_nonblocking(&mut empty_buf).expect("tun read");
        assert_eq!(&empty_buf[..n_tun], &reply);

        tun.close();
        assert!(!tun.is_open());
        assert!(tun.configure_ip("10.8.0.2", "255.255.255.0").is_err());
    }
}
