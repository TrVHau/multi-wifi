# MultiWiFi Security Architecture & Privilege Model

This document outlines the security architecture, threat model, privilege boundary, and defense-in-depth mitigations implemented across the GTK4 GUI (`multiwifi-gui`) and the Rust privileged system daemon (`multiwifi-daemon`).

---

## 1. Threat Model & Protected Assets

### Protected Assets
- **System Routing Integrity:** Preventing unprivileged modification of host default routes, policy routing tables, and RPDB rules.
- **Firewall & NAT State:** Ensuring existing user/container firewall rules (UFW, firewalld, Docker, Tailscale) are never flushed or disrupted.
- **DNS Resolver Integrity:** Preventing unauthorized hijacking of `/etc/resolv.conf` / NetworkManager configuration or symlink traversal attacks.
- **Wi-Fi & Tunnel Credentials:** Keeping Wi-Fi passphrases and MPTCP bonding tokens redacted from logs (`BondingProfile::fmt` redacts tokens), configuration files, and diagnostics.
- **Host Resource Limits:** Preventing local DoS via unbounded operation queues, parser buffers, DNS arrays, or file descriptor leaks.

### Threat Vectors & Mitigations
| Threat Vector | Severity | Vulnerability ID | Implemented Mitigation |
| :--- | :---: | :---: | :--- |
| **Unauthorized network mutation by unprivileged app** | High | CWE-250 | Polkit authorization (`io.github.multiwifi.modify`) enforced asynchronously by `multiwifi-daemon` over System D-Bus. |
| **DNS Symlink Traversal** | High | CWE-59 | Checks `symlink_metadata` + temporary file `O_NOFOLLOW \| O_CLOEXEC` + atomic `rename()` on `/etc/NetworkManager/conf.d/99-multiwifi-dns.conf` and `/run/multiwifi/resolv.conf.bak`, while preserving symlinked `/etc/resolv.conf`. |
| **Memory corruption / raw netlink & packet bugs in root daemon** | High | CWE-119/416 | Core networking & system daemon rewritten in Rust (`#![deny(unsafe_code)]` on protocol, config, journal, scheduler, path monitor, bonding client, D-Bus service; isolated `# SAFETY`-documented `unsafe` only in `libnftables` & `tun_device` FFI). |
| **Use-After-Free (UAF) in GTK4 async dialogs** | High | CWE-416 | Refcounted async task tracking (`active_async_tasks`, `is_destroyed`) in `src/gui/window.c` preventing callbacks on destroyed widgets/dialogs. |
| **nftables Masquerade Table Leak** | High | CWE-772 | Unconditional teardown of both `table inet multiwifi` and `table ip multiwifi` without flushing unrelated rules. |
| **Tunnel Packet Spoofing / Forged Loopback** | High | CWE-290 | `is_valid_inbound_ip_packet()` validates IPv4 length/IHL and drops loopback (`127.0.0.0/8`), multicast (`224.0.0.0/4`), broadcast, and unspecified sources. |
| **Memory Exhaustion via Long-Running Operations** | Medium | CWE-400 | Bounded FIFO queue (`MAX_OPERATION_HISTORY = 50`) in `OpManager`, 4 MiB framing buffer cap, 16-entry DNS limit, and 1.5s diagnostics rate-limiting. |
| **File Descriptor Leak on Exec** | Medium | CWE-772 | Mandatory `O_CLOEXEC` / `SOCK_CLOEXEC` across TUN, netlink, and socket descriptors. |
| **Unprivileged GUI Dependency Creep** | Medium | CWE-250 | `multiwifi-gui` links only `multiwifi-gui-support` + `GTK4` (verified via `ldd`; zero `libpolkit` or `libnftables` linkage). |

---

## 2. Process Separation & Privilege Boundary

```text
+------------------------------------------------------------------------+
| UNPRIVILEGED USER SPACE (User Session)                                 |
|                                                                        |
|   +-----------------------+              +-------------------------+   |
|   |   multiwifi-gui       |              |   CLI / Script (gdbus)  |   |
|   |   - C11 + GTK4        |              |   - Unprivileged UID    |   |
|   |   - Unprivileged UID  |              |   - Read-only / Polkit  |   |
|   +-----------------------+              +-------------------------+   |
+-----------------------------------|------------------------------------+
                                    | System D-Bus
                                    | (io.github.multiwifi.Daemon1)
+-----------------------------------|------------------------------------+
| PRIVILEGED SYSTEM DAEMON (Rust: crates/multiwifi-daemon)               |
|                                   v                                    |
|                  Polkit CheckAuthorization (zbus)                      |
|                      (io.github.multiwifi.modify)                      |
|                                   |                                    |
|             +---------------------+---------------------+              |
|             | (Authorized)                              | (Denied)     |
|             v                                           v              |
|   +---------------------------------------+    +-------------------+   |
|   |          multiwifi-daemon             |    | D-Bus Error:      |   |
|   | - Sandboxed via systemd               |    | .NotAuthorized    |   |
|   | - rtnetlink / nftables / TUN / DNS    |    +-------------------+   |
|   +---------------------------------------+                            |
+------------------------------------------------------------------------+
```

---

## 3. Polkit Authorization Model

Mutating operations require authorization for Polkit action `io.github.multiwifi.modify`:

- **Read-Only / Unauthenticated Operations:**
  - `GetVersion`, `GetCapabilities`, `GetDevices`, `GetPaths`, `GetStatus`, `GetDNS`, `GetDiagnostics`, `RunDiagnostics` *(rate-limited at 1.5s cooldown)*.
- **Mutating Operations (Polkit Enforced on System Bus):**
  - `StartLoadBalancing`, `StopLoadBalancing`, `StartBonding`, `StopBonding`, `SetPathWeight`, `SetDNS`, `ResetDNS`, `StopDaemon`.

### Active Session Policy (`data/io.github.multiwifi.rules`)
```javascript
polkit.addRule(function(action, subject) {
    if (action.id == "io.github.multiwifi.modify") {
        if (subject.local && subject.active && (subject.isInGroup("wheel") || subject.isInGroup("sudo"))) {
            return polkit.Result.YES;
        }
        return polkit.Result.AUTH_ADMIN_KEEP;
    }
});
```

---

## 4. Systemd Service Sandboxing

`/usr/lib/systemd/system/multiwifi-daemon.service` applies strict unit sandboxing:
- `ProtectSystem=strict`: mounts `/usr`, `/boot`, and root filesystem read-only.
- `ProtectHome=true`: isolates `/home` and `/root`.
- `ReadWritePaths=/etc /run` and `RuntimeDirectory=multiwifi`: limits persistent/runtime writes strictly to DNS drop-ins, backup state, and routing journals.
- `CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW`.

---

## 5. Network Isolation & Recovery

1. **Isolated Policy Tables:** MultiWiFi touches only routing tables `10001+` (`BASE_TABLE_ID = 10000`), leaving `main` and `default` routing intact aside from explicit `/32` tunnel loop-prevention pinning.
2. **Dual-Table `nftables` Isolation:** Manages `table inet multiwifi` and `table ip multiwifi` only; `nft flush ruleset` is prohibited.
3. **Journal Recovery on Startup:** Daemon startup automatically executes `RoutingManager::recover()` on `/run/multiwifi/journals/*.journal` and tears down stale `multiwifi` nftables tables left by any prior crash.
