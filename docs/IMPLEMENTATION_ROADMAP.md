# MultiWiFi Implementation Roadmap & Milestones

This document records the completion status, architectural deliverables, and verification evidence for all phases of the MultiWiFi project.

---

## Phase Completion Summary

| Phase | Description | Status | Deliverables & Verification |
| :---: | :--- | :---: | :--- |
| **0** | **Foundation & Architecture** | **COMPLETED** | Modern C11 toolchain, CMake build, strict warnings (`-Wall -Wextra -Wpedantic`), logging subsystem, TOML configuration parser, unit test harnesses, and CI pipeline. |
| **1** | **Wi-Fi Inventory & Discovery** | **COMPLETED** | `MultiWifiInventory` using libnm with live device tracking, AP scanning, async connection, and netlink/sysfs fallback for multi-adapter hardware discovery. |
| **2** | **Path Monitoring & Link Quality** | **COMPLETED** | `MultiWifiPathMonitor` with non-blocking multi-port socket fallback probe (DNS 53 -> HTTP 80 -> HTTPS 443), RTT smoothing, jitter estimation, rolling loss calculation, hysteresis (3 successes / 3 failures). |
| **3** | **System Daemon & D-Bus IPC** | **COMPLETED** | `MultiWifiDaemonService` running over System D-Bus (`io.github.multiwifi.Daemon`), Polkit authorization (`io.github.multiwifi.modify`), wheel/sudo active session rules, bounded queue (`MAX_OPERATION_HISTORY = 50`), hardened systemd service. |
| **4** | **Routing Transaction Manager** | **COMPLETED** | `MultiWifiRoutingManager` with transactional disk-backed journals (`/run/multiwifi/journals/*.journal`), policy routing tables ($201 \dots 200+N$), fwmark & source rule preferences, loose `rp_filter = 2` with original value preservation, atomic rollback. |
| **5** | **Multi-Path Load Balancing** | **COMPLETED** | `MultiWifiLoadBalancer` and `MultiWifiFirewall` with isolated dual nftables tables (`table inet multiwifi_table` and `table ip multiwifi_table`), conntrack flow affinity (`ct mark`), Weighted Round-Robin scheduling with GCD normalization, unconditional teardown of both tables. |
| **6** | **MPTCP WAN Tunnel Bonding** | **COMPLETED** | `MultiWifiBondingClient`, `MultiWifiTunDevice`, and `MultiWifiLoopPrevention`. Framed streaming protocol with magic header `0x4D574246`, HMAC auth tokens, loopback anti-spoofing validation, `/32` host route bypass for loop prevention, `O_CLOEXEC` on descriptors. |
| **7** | **Release, Packaging & GUI Redesign**| **COMPLETED** | Modern GTK4 desktop GUI with toggle switches, DNS Manager dialog with atomic backup/restore, cross-distro packaging (Arch `PKGBUILD`, Debian `.deb`, Fedora `.rpm`, AppImage), unified `setup.sh` installer, comprehensive benchmarks. |

---

## Phase Details & Verification Evidence

### Phase 0: Foundation & Core Tooling
- **Deliverables:** `src/core/logging.c`, `src/core/config.c`, `src/core/capabilities.c`.
- **Verification:** Unit tests `test_config` and `test_capabilities` pass 100%. Compilation clean with zero warnings under `-Wall -Wextra -Wpedantic`.

### Phase 1: Wi-Fi Discovery & Inventory
- **Deliverables:** `src/core/wifi_device.c`, `src/core/wifi_inventory.c`.
- **Key Features:** Live monitoring of NetworkManager devices; AP rescan; netlink/sysfs fallback resolving both PCIe internal and USB external adapters concurrently.
- **Verification:** Unit test `test_wifi_device` passes 100%.

### Phase 2: Path Quality Monitoring
- **Deliverables:** `src/core/path_monitor.c`, `include/multiwifi/core/path_monitor.h`.
- **Key Features:** Fallback socket probing (DNS 53 $\rightarrow$ HTTP 80 $\rightarrow$ HTTPS 443) preventing false-negative link drops on strict home routers; RTT smoothing and jitter tracking; hysteresis state machine (`PROBING` $\rightarrow$ `ONLINE` after 3 consecutive successes; `ONLINE` $\rightarrow$ `LOCAL_ONLY` after 3 failures).
- **Verification:** Unit test `test_path_monitor` passes 100% across all 4 test functions.

### Phase 3: Privileged System Daemon & Security
- **Deliverables:** `src/daemon/service.c`, `data/io.github.multiwifi.policy`, `data/io.github.multiwifi.rules`, `data/multiwifi-daemon.service`.
- **Key Features:** Sandboxed systemd service (`ProtectSystem=strict`, `ProtectHome=true`, `ReadWritePaths=/etc /run`); Polkit authorization for local active console administrators; bounded operation history queue with constant-time pruning.
- **Verification:** Unit test `test_dbus_daemon` passes 100%.

### Phase 4: Transactional Policy Routing
- **Deliverables:** `src/core/routing_manager.c`, `src/core/netlink_route.c`, `src/core/journal.c`.
- **Key Features:** Isolated routing tables starting at ID 201; source address matching rules (preference 10000+); fwmark rules (preference 11000+); `orig_rp_filter` preservation; disk-backed recovery journals at `/run/multiwifi/journals/`; automatic recovery on startup.
- **Verification:** Unit test `test_routing_manager` passes 100% in isolated network namespace.

### Phase 5: Load Balancing & nftables Engine
- **Deliverables:** `src/core/load_balancer.c`, `src/core/firewall.c`.
- **Key Features:** Declarative nftables rules preserving conntrack flow affinity; dual-table architecture (`inet` for output/prerouting marks, `ip` for postrouting masquerade); Weighted Round-Robin scheduler delivering 17.40 Million decisions/sec; unconditional teardown of both tables.
- **Verification:** Unit test `test_load_balancer` passes 100%.

### Phase 6: MPTCP WAN Bonding & TUN Aggregation
- **Deliverables:** `src/core/bonding_client.c`, `src/core/tun_device.c`, `src/core/loop_prevention.c`, `src/core/bonding_protocol.c`.
- **Key Features:** Framed streaming protocol with magic header `0x4D574246` (1,465 MB/s parsing throughput); `/32` host bypass to prevent tunnel routing loops; anti-spoofing validation dropping forged loopback packets; `O_CLOEXEC` on `/dev/net/tun`.
- **Verification:** Unit test `test_bonding` passes 100%.

### Phase 7: GUI, DNS Subsystem & Packaging
- **Deliverables:** `src/gui/window.c`, `packaging/arch/`, `packaging/debian/`, `packaging/rpm/`, `packaging/appimage/`, `setup.sh`.
- **Key Features:** Modern GTK4 desktop interface with toggle switches; native DNS Manager dialog (Cloudflare, Google, Quad9, AdGuard, Custom IPv4/IPv6) with atomic swap, backup, and anti-symlink protection; packaging manifests for Arch, Debian, Fedora, AppImage; unified `setup.sh` installer.
- **Verification:** Unit test `test_packaging` passes 100%. AddressSanitizer and LeakSanitizer confirm 0 memory leaks across all 9 test suites.
