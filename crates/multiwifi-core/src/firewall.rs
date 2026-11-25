#![allow(clippy::cast_possible_truncation)]

use std::ffi::{CStr, CString};
use std::fmt::Write as _;
use std::os::raw::c_int;
use std::ptr::NonNull;
use std::sync::Mutex;
use thiserror::Error;

const NFT_TABLE_NAME: &str = "multiwifi";
const NFT_CTX_DEFAULT: u32 = 0;

mod ffi {
    use std::os::raw::{c_char, c_int};

    #[repr(C)]
    pub struct nft_ctx {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn nft_ctx_new(flags: u32) -> *mut nft_ctx;
        pub fn nft_ctx_free(ctx: *mut nft_ctx);
        pub fn nft_ctx_buffer_output(ctx: *mut nft_ctx) -> c_int;
        pub fn nft_ctx_buffer_error(ctx: *mut nft_ctx) -> c_int;
        pub fn nft_ctx_get_error_buffer(ctx: *mut nft_ctx) -> *const c_char;
        pub fn nft_run_cmd_from_buffer(ctx: *mut nft_ctx, buf: *const c_char) -> c_int;
    }
}

#[derive(Debug, Error)]
pub enum FirewallError {
    #[error("invalid firewall argument: {0}")]
    InvalidArgument(&'static str),

    #[error("failed to initialize libnftables context")]
    ContextInitFailed,

    #[error("nftables command failed (rc={rc}): {message}")]
    Nftables { rc: c_int, message: String },
}

/// RAII wrapper ensuring `nft_ctx` is always freed via `nft_ctx_free`.
struct NftContext {
    ptr: NonNull<ffi::nft_ctx>,
}

impl NftContext {
    fn new() -> Result<Self, FirewallError> {
        // SAFETY: `nft_ctx_new(NFT_CTX_DEFAULT)` allocates an opaque libnftables context
        // returning NULL on allocation failure, checked immediately below.
        let raw = unsafe { ffi::nft_ctx_new(NFT_CTX_DEFAULT) };
        let Some(ptr) = NonNull::new(raw) else {
            return Err(FirewallError::ContextInitFailed);
        };
        // SAFETY: `ptr` is non-null and uniquely owned by this `NftContext`.
        unsafe {
            ffi::nft_ctx_buffer_output(ptr.as_ptr());
            ffi::nft_ctx_buffer_error(ptr.as_ptr());
        }
        Ok(Self { ptr })
    }

    fn run_cmd(&mut self, script: &str) -> Result<(), (c_int, String)> {
        let Ok(c_buf) = CString::new(script) else {
            return Err((-1, "nftables script contains interior NUL byte".to_owned()));
        };
        // SAFETY: `self.ptr` is valid and `c_buf` is a valid NUL-terminated C string.
        let rc = unsafe { ffi::nft_run_cmd_from_buffer(self.ptr.as_ptr(), c_buf.as_ptr()) };
        if rc == 0 {
            Ok(())
        } else {
            Err((rc, self.error_buffer()))
        }
    }

    fn error_buffer(&mut self) -> String {
        // SAFETY: `self.ptr` is valid; `nft_ctx_get_error_buffer` returns a borrowed
        // C string valid until the next libnftables call or context destruction.
        let err_ptr = unsafe { ffi::nft_ctx_get_error_buffer(self.ptr.as_ptr()) };
        if err_ptr.is_null() {
            return String::new();
        }
        // SAFETY: Checked non-null; string is NUL-terminated by libnftables.
        unsafe { CStr::from_ptr(err_ptr) }
            .to_string_lossy()
            .into_owned()
    }
}

impl Drop for NftContext {
    fn drop(&mut self) {
        // SAFETY: `self.ptr` was allocated by `nft_ctx_new` and has not been freed yet.
        unsafe { ffi::nft_ctx_free(self.ptr.as_ptr()) };
    }
}

fn build_inet_rules(marks: &[u32]) -> Result<String, FirewallError> {
    if marks.is_empty() {
        return Err(FirewallError::InvalidArgument(
            "no marks provided for load balancing",
        ));
    }
    if marks.contains(&0) {
        return Err(FirewallError::InvalidArgument(
            "fwmark must be non-zero",
        ));
    }

    let mut rules = String::with_capacity(1024);
    let _ = writeln!(rules, "add table inet {NFT_TABLE_NAME}");
    let _ = writeln!(rules, "flush table inet {NFT_TABLE_NAME}");
    let _ = writeln!(rules, "table inet {NFT_TABLE_NAME} {{");
    let _ = writeln!(rules, "    chain output {{");
    let _ = writeln!(
        rules,
        "        type filter hook output priority -150; policy accept;"
    );
    let _ = writeln!(rules, "        oif \"lo\" accept");
    let _ = writeln!(
        rules,
        "        ip daddr {{ 127.0.0.0/8, 224.0.0.0/4, 255.255.255.255 }} accept"
    );
    let _ = writeln!(
        rules,
        "        ip6 daddr {{ ::1, fe80::/10, ff00::/8 }} accept"
    );
    let _ = writeln!(
        rules,
        "        ip daddr {{ 192.168.0.0/16, 10.0.0.0/8, 172.16.0.0/12 }} udp dport 53 accept"
    );
    let _ = writeln!(rules, "        udp dport {{ 67, 68, 546, 547 }} accept");
    let _ = writeln!(rules, "        meta mark & 0xff00 != 0 accept");
    let _ = writeln!(rules, "        ip6 version 6 drop");
    let _ = writeln!(
        rules,
        "        ct mark & 0xff00 != 0 meta mark set ct mark accept"
    );

    if marks.len() == 1 {
        let single_mark = marks[0];
        let _ = writeln!(rules, "        meta mark set {single_mark:#x}");
    } else {
        const NUM_BUCKETS: usize = 64;
        let mut distinct = Vec::new();
        for &m in marks {
            if !distinct.contains(&m) {
                distinct.push(m);
            }
        }

        for target_mark in distinct {
            let mut buckets = String::new();
            let mut count = 0usize;
            for b in 0..NUM_BUCKETS {
                let slot = (b * marks.len()) / NUM_BUCKETS;
                if marks[slot] == target_mark {
                    if count > 0 {
                        buckets.push_str(", ");
                    }
                    let _ = write!(buckets, "{b}");
                    count += 1;
                }
            }
            if count > 0 {
                let _ = writeln!(
                    rules,
                    "        tcp sport & 0x3f == {{ {buckets} }} meta mark set {target_mark:#x}"
                );
                let _ = writeln!(
                    rules,
                    "        udp sport & 0x3f == {{ {buckets} }} meta mark set {target_mark:#x}"
                );
            }
        }

        let default_mark = marks[0];
        let _ = writeln!(rules, "        meta mark 0 meta mark set {default_mark:#x}");
    }

    let _ = writeln!(
        rules,
        "        meta mark & 0xff00 != 0 ct mark set meta mark"
    );
    let _ = writeln!(rules, "    }}");
    let _ = writeln!(rules, "}}");
    Ok(rules)
}

fn build_ip_nat_rules() -> String {
    format!(
        "add table ip {NFT_TABLE_NAME}\n\
         flush table ip {NFT_TABLE_NAME}\n\
         table ip {NFT_TABLE_NAME} {{\n\
             chain postrouting {{\n\
                 type nat hook postrouting priority 100; policy accept;\n\
                 oif != \"lo\" meta mark != 0 masquerade\n\
             }}\n\
         }}\n"
    )
}

/// Manages `MultiWiFi`-owned nftables tables (`inet multiwifi` & `ip multiwifi`) without flushing global rules.
#[derive(Debug, Default)]
pub struct FirewallManager {
    active: Mutex<bool>,
}

impl FirewallManager {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Atomically replaces the `MultiWiFi` `inet` filter and `ip` NAT masquerade tables.
    pub fn apply(&self, marks: &[u32]) -> Result<(), FirewallError> {
        let inet_rules = build_inet_rules(marks)?;
        let mut ctx = NftContext::new()?;
        if let Err((rc, msg)) = ctx.run_cmd(&inet_rules) {
            let clean = if msg.trim().is_empty() {
                "unknown libnftables error".to_owned()
            } else {
                msg
            };
            return Err(FirewallError::Nftables { rc, message: clean });
        }

        // Phase 2: Postrouting SNAT masquerade (best-effort if nat kernel module absent in namespace)
        if let Ok(mut nat_ctx) = NftContext::new() {
            let nat_rules = build_ip_nat_rules();
            let _ = nat_ctx.run_cmd(&nat_rules);
        }

        let mut active = self
            .active
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        *active = true;
        Ok(())
    }

    /// Deletes only the `MultiWiFi`-owned `inet` and `ip` tables.
    pub fn teardown(&self) -> Result<(), FirewallError> {
        let mut ctx = NftContext::new()?;
        let _ = ctx.run_cmd(&format!("delete table inet {NFT_TABLE_NAME}\n"));
        let _ = ctx.run_cmd(&format!("delete table ip {NFT_TABLE_NAME}\n"));

        let mut active = self
            .active
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        *active = false;
        Ok(())
    }

    /// Verifies whether `MultiWiFi`-owned nftables tables exist in the current network namespace.
    pub fn verify(&self) -> Result<bool, FirewallError> {
        let mut ctx = NftContext::new()?;
        if ctx
            .run_cmd(&format!("list table inet {NFT_TABLE_NAME}\n"))
            .is_ok()
        {
            return Ok(true);
        }
        let first_err = ctx.error_buffer();
        if ctx
            .run_cmd(&format!("list table ip {NFT_TABLE_NAME}\n"))
            .is_ok()
        {
            return Ok(true);
        }
        let combined_err = if first_err.is_empty() {
            ctx.error_buffer()
        } else {
            first_err
        };

        if combined_err.contains("No such file or directory")
            || combined_err.contains("does not exist")
        {
            return Ok(false);
        }

        Err(FirewallError::Nftables {
            rc: -1,
            message: if combined_err.trim().is_empty() {
                "permission denied or unknown error".to_owned()
            } else {
                combined_err
            },
        })
    }

    #[must_use]
    pub fn is_active(&self) -> bool {
        *self
            .active
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_marks_rejected() {
        let fw = FirewallManager::new();
        assert!(matches!(
            fw.apply(&[]).unwrap_err(),
            FirewallError::InvalidArgument(_)
        ));
        assert!(matches!(
            fw.apply(&[0]).unwrap_err(),
            FirewallError::InvalidArgument(_)
        ));
    }

    #[test]
    fn rules_builder_generates_ct_affinity_and_flush() {
        let script = build_inet_rules(&[0x1001, 0x1002]).expect("build");
        assert!(script.contains("flush table inet multiwifi"));
        assert!(script.contains("ct mark & 0xff00 != 0 meta mark set ct mark accept"));
        assert!(script.contains("meta mark & 0xff00 != 0 ct mark set meta mark"));
        assert!(script.contains("0x1001"));
        assert!(script.contains("0x1002"));

        let nat = build_ip_nat_rules();
        assert!(nat.contains("add table ip multiwifi"));
        assert!(nat.contains("flush table ip multiwifi"));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn netns_firewall_lifecycle() {
        // Test in a forked isolated user+net namespace so the host process namespace is untouched.
        // Note: on systems where AppArmor/kernel restricts unprivileged nftables inside userns,
        // Operation not permitted is treated identically to test_load_balancer.c (skip safely).
        use nix::sched::{unshare, CloneFlags};
        use nix::sys::wait::{waitpid, WaitStatus};
        use nix::unistd::{fork, ForkResult};
        use std::io::Write;

        let uid = nix::unistd::getuid();
        let gid = nix::unistd::getgid();

        match unsafe { fork() } {
            Ok(ForkResult::Child) => {
                if unshare(CloneFlags::CLONE_NEWUSER | CloneFlags::CLONE_NEWNET).is_err() {
                    std::process::exit(0);
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

                let fw = FirewallManager::new();
                match fw.verify() {
                    Ok(exists) => {
                        if exists {
                            std::process::exit(2);
                        }
                    }
                    Err(_) => std::process::exit(0),
                }

                if let Err(err) = fw.apply(&[0x1001]) {
                    let msg = err.to_string();
                    if msg.contains("Operation not permitted")
                        || msg.contains("Permission denied")
                    {
                        std::process::exit(0);
                    }
                    std::process::exit(3);
                }
                if !fw.is_active() || !matches!(fw.verify(), Ok(true)) {
                    std::process::exit(4);
                }
                if fw.apply(&[0x1001, 0x1002]).is_err() {
                    std::process::exit(5);
                }
                if fw.teardown().is_err() || fw.is_active() {
                    std::process::exit(6);
                }
                if !matches!(fw.verify(), Ok(false)) {
                    std::process::exit(7);
                }
                std::process::exit(0);
            }
            Ok(ForkResult::Parent { child }) => {
                let status = waitpid(child, None).expect("waitpid");
                assert_eq!(status, WaitStatus::Exited(child, 0));
            }
            Err(e) => panic!("fork failed: {e}"),
        }
    }
}
