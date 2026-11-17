# MultiWiFi Security Architecture & Privilege Model

This document outlines the security architecture, threat model, authorization mechanisms, and defense-in-depth mitigations implemented in MultiWiFi.

---

## 1. Threat Model & Protected Assets

### Protected Assets
- **System Routing Integrity:** Preventing unauthorized modification of host default gateways, policy routing tables, and interface metrics.
- **Firewall & NAT State:** Ensuring user/container firewall rules (UFW, firewalld, Docker, Tailscale) are never flushed or disrupted.
- **DNS Resolver Integrity:** Preventing unauthorized hijacking of `/etc/resolv.conf` or symlink redirection attacks.
- **Wi-Fi & Tunnel Credentials:** Keeping Wi-Fi passphrases and MPTCP aggregation tokens confidential and redacted from logs, core dumps, and D-Bus interfaces.
- **Host Resource Limits:** Preventing local denial-of-service (DoS) via unbounded operation queues or file descriptor leaks.

### Threat Vectors & Mitigations
| Threat Vector | Severity | Vulnerability ID | Implemented Mitigation |
| :--- | :---: | :---: | :--- |
| **Unauthorized network mutation by unprivileged app** | High | CWE-250 | Polkit authorization (`io.github.multiwifi.modify`) mediated strictly over System D-Bus. |
| **DNS Symlink Traversal (`/etc/resolv.conf`)** | High | CWE-59 | Atomic file replacement with `O_NOFOLLOW` and `lstat()` checks prior to writing. |
| **Use-After-Free (UAF) in GTK4 async dialogs** | High | CWE-416 | Refcounted async task tracking (`active_async_tasks`, `is_destroyed`) preventing callback execution on destroyed dialogs. |
| **nftables Masquerade Table Leak** | High | CWE-772 | Unconditional teardown of both `table inet multiwifi_table` and `table ip multiwifi_table`. |
| **Tunnel Packet Spoofing / Forged Loopback** | High | CWE-290 | Kernel-level header validation; drops any packet arriving via TUN with `127.0.0.0/8` source. |
| **Memory Exhaustion via Long-Running Operations** | Medium | CWE-400 | Bounded FIFO queue (`MULTIWIFI_MAX_OPERATION_HISTORY = 50`) with auto-pruning. |
| **File Descriptor Leak on Exec** | Medium | CWE-772 | Mandatory `O_CLOEXEC` on `/dev/net/tun` and `SOCK_CLOEXEC` on all ioctl/netlink sockets. |
| **Netlink CIDR Prefix Injection** | Medium | CWE-20 | Strict subnet prefix bounds checking ($0 \le \text{prefix} \le 32$) prior to netlink dispatch. |
| **System sysctl `rp_filter` pollution** | Medium | CWE-284 | Original `orig_rp_filter` saved per interface; only restored to strict mode if previously set to strict. |

---

## 2. Process Separation & Privilege Boundary

MultiWiFi enforces a strict privilege boundary separating the presentation layer from network mutations:

```text
+------------------------------------------------------------------------+
| UNPRIVILEGED USER SPACE (User Session)                                 |
|                                                                        |
|   +-----------------------+              +-------------------------+   |
|   |   multiwifi-gui       |              |   CLI / Script (busctl) |   |
|   |   - Unprivileged UID  |              |   - Unprivileged UID    |   |
|   |   - GTK4 / Libadwaita |              |   - Read-only queries   |   |
|   |   - No root/sudo      |              |                         |   |
|   +-----------------------+              +-------------------------+   |
+-----------------------------------|------------------------------------+
                                    | System D-Bus
                                    | (io.github.multiwifi.Daemon)
+-----------------------------------|------------------------------------+
| PRIVILEGED SYSTEM DAEMON (Root / CAP_NET_ADMIN)                        |
|                                   v                                    |
|                       Polkit Authorization Engine                      |
|                      (io.github.multiwifi.modify)                      |
|                                   |                                    |
|             +---------------------+---------------------+              |
|             | (Authorized)                              | (Denied)     |
|             v                                           v              |
|   +---------------------------------------+    +-------------------+   |
|   |          multiwifi-daemon             |    | Return Error:     |   |
|   | - Sandboxed via systemd               |    | .NotAuthorized    |   |
|   | - ReadWritePaths=/etc /run            |    +-------------------+   |
|   | - ProtectSystem=strict, ProtectHome   |                            |
|   +---------------------------------------+                            |
+------------------------------------------------------------------------+
```

1. **User GUI (`multiwifi-gui`):**
   - Runs strictly under the user's unprivileged UID in Wayland/X11.
   - Has zero access to raw sockets, TUN devices, routing tables, or netfilter.
   - Communicates with the daemon exclusively via standard System D-Bus method calls.
2. **System Service (`multiwifi-daemon`):**
   - Runs as a systemd service under root or with restricted Linux capabilities (`CAP_NET_ADMIN`, `CAP_NET_RAW`).
   - Validates all caller arguments, addresses, and interface names before execution.

---

## 3. Polkit Authorization Model

Mutating operations require authorization through Polkit action `io.github.multiwifi.modify`:

- **Read-Only Operations (No Auth Required):**
  - `GetVersion`, `GetCapabilities`, `GetDevices`, `GetPaths`, `GetStatus`, `GetDNS`, `GetDiagnostics`.
- **Mutating Operations (Polkit Enforced):**
  - `StartLoadBalancing`, `StopLoadBalancing`, `StartBonding`, `StopBonding`, `SetPathWeight`, `SetDNS`, `ResetDNS`, `RunDiagnostics`.

### Active Session Policy (`io.github.multiwifi.rules`)
To provide a smooth experience across both desktop environments (GNOME, KDE) and lightweight window managers (Niri, Sway, Hyprland) where a GUI authentication agent may not be active:
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
- Local active session administrators (in group `wheel` or `sudo`) are granted access without repeated password prompts.
- Remote sessions, inactive sessions, or standard non-admin users must authenticate as administrator (`AUTH_ADMIN_KEEP`).

---

## 4. Systemd Service Sandboxing

The daemon service unit (`/usr/lib/systemd/system/multiwifi-daemon.service`) applies comprehensive hardening directives:

```ini
[Service]
Type=dbus
BusName=io.github.multiwifi.Daemon
ExecStart=/usr/bin/multiwifi-daemon
Restart=on-failure
RestartSec=3s

# Filesystem Sandboxing
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/etc /run
RuntimeDirectory=multiwifi

# Kernel & Capabilities
ProtectKernelTunables=false
ProtectKernelModules=false
CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW
```
- **`ProtectSystem=strict`:** Entire `/usr`, `/boot`, and root filesystem are mounted read-only.
- **`ProtectHome=true`:** User directories (`/home`, `/root`) are completely inaccessible.
- **`ReadWritePaths=/etc /run`:** Write access is strictly constrained to `/etc` (for atomic DNS configuration) and `/run` (for D-Bus runtime and recovery journals).
- **`CapabilityBoundingSet`:** Limits process capabilities to only network administration and raw packet manipulation.

---

## 5. DNS Security & Symlink Protection

When configuring DNS via `SetDNS` or `ResetDNS`, the daemon protects against symlink traversal attacks (`CWE-59`):
1. **Symlink Inspection:** Checks `/etc/resolv.conf` using `lstat()`. If it is a symbolic link pointing to an untrusted or non-resolv path, the operation safely manages resolution without traversing arbitrary filesystem locations.
2. **Atomic Swapping:** New configuration is written to a randomized temporary file in `/etc/` using `O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC`, flushed with `fsync()`, and atomically renamed to `/etc/resolv.conf`.
3. **Automated Backup & Restoration:** An uncorrupted copy of the original resolver configuration is stored at `/etc/resolv.conf.multiwifi-backup` and restored on `ResetDNS` or daemon teardown.

---

## 6. Network Isolation & Non-Destructive Operation

MultiWiFi operates under a strict "clean ownership" guarantee:
1. **Isolated Routing Tables:** Routing rules only direct packets into MultiWiFi-managed tables ($201 \dots 200+N$), leaving system tables (`main`, `default`) untouched.
2. **Dual-Table nftables Isolation:**
   - Flow filtering: `table inet multiwifi_table`
   - Masquerading: `table ip multiwifi_table`
   - No global tables are ever flushed (`nft flush ruleset` is prohibited).
3. **Transactional Journaling:** Every routing and netfilter mutation is recorded in `/run/multiwifi/journals/*.journal`. If the daemon crashes or the system loses power, the daemon automatically discovers stale journals on boot and cleanly unrolls all rules.
4. **Anti-Spoofing & Leak Prevention:**
   - Ingress packets over TUN with spoofed source addresses (`127.0.0.1` or loopback) are dropped immediately.
   - Tunnel endpoint routes are installed with `/32` host masks to prevent recursive routing loops.

---

## 7. Memory Safety & Concurrency

- **Zero Memory Leaks:** Verified continuously with AddressSanitizer (ASan), LeakSanitizer (LSan), and UndefinedBehaviorSanitizer (UBSan).
- **Asynchronous Dialog Lifetime:** In the GTK4 GUI, dialog structures (`WifiSelectDialog`, `DnsConfigDialog`) maintain active asynchronous task counts. Closing the dialog cancels ongoing requests without freeing the underlying structure until all in-flight GLib callbacks return, preventing Use-After-Free crashes.
- **Bounded Resource Consumption:** Operation queues are limited to 50 entries, and old entries are pruned to ensure constant memory footprint over months of uptime.
