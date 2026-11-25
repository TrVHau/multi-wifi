# MultiWiFi Implementation Roadmap & Milestones

This document records the completed milestones and architectural evolution of MultiWiFi from its initial C11 implementation (Phases 0–7) through the completed Strangler Fig Rust migration (Phases 0–5, [ADR-013](ADR/ADR-013-rust-migration.md)).

---

## Strangler Fig Rust Migration Status (ADR-013)

| Migration Phase | Scope | Status | Deliverables & Verification |
| :---: | :--- | :---: | :--- |
| **Phase 0** | **Workspace & CMake Boundary Split** | **COMPLETED** | Root Cargo workspace (`Cargo.toml`, `edition = "2021"`, strict `clippy::pedantic`, `unsafe_op_in_unsafe_fn = "deny"`), updated `AGENTS.md`, `ADR-001` & `ADR-013`, split CMake `multiwifi-gui-support` so `multiwifi-gui` carries zero `polkit`/`libnftables` linkage. |
| **Phase 1** | **Pure Core Crates (`multiwifi-core`)** | **COMPLETED** | `config.rs`, `load_balancer.rs`, `bonding_protocol.rs` (`0x4d574631` `"MWF1"`), and `journal.rs` (`#![deny(unsafe_code)]`, 18 unit tests passing). |
| **Phase 2** | **Network Engine & Kernel Primitives** | **COMPLETED** | `capabilities.rs`, `firewall.rs` (`libnftables` FFI + `table inet/ip multiwifi`), `netlink_route.rs` (`rtnetlink` tables `10001+`), `loop_prevention.rs`, `routing_manager.rs`, `path_monitor.rs`, `tun_device.rs` (`mwtun0`), `bonding_client.rs` (38 core tests passing). |
| **Phase 3** | **Privileged System Daemon (`multiwifi-daemon`)** | **COMPLETED** | Async `tokio` + `zbus` service implementing `io.github.multiwifi.Daemon1` (`service.rs`), Polkit v1 check (`polkit.rs`), `NetworkManager` D-Bus inventory + kernel fallback (`nm_client.rs`), bounded `OpManager` (`operation.rs`), symlink-safe DNS (`dns.rs`); wired into CMake, `setup.sh`, CI, and packaging. |
| **Phase 4** | **Integration Parity & Cutover Stabilization** | **COMPLETED** | Runtime `/etc/multiwifi/multiwifi.conf` (`MULTIWIFI_CONFIG_PATH`) loading + `NmInventory` $\rightarrow$ `PathMonitor` live sync (`service.rs`), live cross-language C `GDBusConnection` $\leftrightarrow$ Rust daemon binary parity suite (`test_rust_parity` covering introspection XML, IPC contract, signals, DNS hardening, and Polkit denial across `10/10` CTest suites + `48/48` Rust tests), `rustfmt` check, and GCC + Clang + ASan/UBSan multi-toolchain CI hardening. |
| **Phase 5** | **Legacy C Core Deprecation & Release Cleanup** | **COMPLETED** | Retired legacy C daemon (`src/daemon/*`), all 11 migrated backend C modules (`src/core/{config,operation,journal,netlink_route,routing_manager,load_balancer,firewall,bonding_protocol,tun_device,loop_prevention,bonding_client}.c` + headers), 5 duplicate backend C tests, and `prompts/PHASE-0.md`; removed unused `serde` runtime dep and `polkit-gobject-1` build dep; simplified `CMakeLists.txt` to `multiwifi-gui-support` + `multiwifi-gui` (C11/GTK4) and Rust `multiwifi-daemon` with 5 focused CTest suites (`test_rust_parity`, `test_packaging`, `test_capabilities`, `test_wifi_device`, `test_path_monitor`) + 48 Rust tests. |

---

## Original Feature Milestones (Phases 0–7)

| Phase | Description | Status | Summary |
| :---: | :--- | :---: | :--- |
| **0** | Foundation & Core Tooling | **COMPLETED** | Logging, strict TOML config parser (`deny_unknown_fields`, secret rejection), and capability probing. |
| **1** | Wi-Fi Discovery & Inventory | **COMPLETED** | `NetworkManager` Wi-Fi adapter discovery, AP scanning, PCIe/USB identification, kernel IPv4 fallback. |
| **2** | Path Quality Monitoring | **COMPLETED** | Multi-port non-blocking socket probe fallback (`53 -> 80 -> 443`), EMA RTT/jitter, rolling loss, 3-step hysteresis. |
| **3** | System Daemon & D-Bus IPC | **COMPLETED** | Versioned System D-Bus service (`io.github.multiwifi.Daemon1`), Polkit authorization (`io.github.multiwifi.modify`), bounded FIFO queue (`50`), hardened systemd unit. |
| **4** | Transactional Policy Routing | **COMPLETED** | Netlink routing tables `10001+`, source rules (`20001+`), fwmark rules (`21001+`), `orig_rp_filter` preservation, INI recovery journals, atomic rollback. |
| **5** | Load Balancing & `nftables` Engine | **COMPLETED** | Dual-table `inet multiwifi` + `ip multiwifi` rules with `ct mark` affinity and GCD-normalized WRR distribution. |
| **6** | MPTCP WAN Bonding & TUN | **COMPLETED** | Framed binary protocol (`0x4d574631`) over `IPPROTO_MPTCP`/TCP fallback, `mwtun0` MTU 1420, `/32` host-route loop prevention, loopback anti-spoofing. |
| **7** | GUI, DNS & Packaging | **COMPLETED** | Modern C11 + GTK4 desktop app (`src/gui/window.c`), DNS Manager dialog with `O_NOFOLLOW` backup/restore, Arch/Debian/Fedora/AppDir packaging, unified `setup.sh`. |
