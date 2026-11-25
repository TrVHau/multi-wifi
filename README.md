<p align="center">
  <img src="data/icons/hicolor/512x512/apps/io.github.multiwifi.MultiWiFi.png" width="140" alt="MultiWiFi Logo" />
</p>

# MultiWiFi

[![CI](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml/badge.svg)](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml)
[![Rust](https://img.shields.io/badge/Daemon%20%26%20Core-Rust%202021-orange.svg)](https://www.rust-lang.org/)
[![C11](https://img.shields.io/badge/GUI-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![GTK4](https://img.shields.io/badge/UI-GTK4%20%7C%20Libadwaita%20Style-green.svg)](https://www.gtk.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-orange.svg)](https://kernel.org/)
[![Security](https://img.shields.io/badge/Security-Polkit%20%2B%20D--Bus-red.svg)](docs/SECURITY.md)

**MultiWiFi** is a native Linux desktop application and privileged system daemon that aggregates and balances network traffic across multiple concurrent Wi-Fi adapters (internal PCIe Wi-Fi, USB Wi-Fi dongles, or tethered mobile 5G hotspots) to improve throughput, steer latency-sensitive flows to healthier links, and provide automatic zero-downtime failover.

Following the Strangler Fig migration architecture ([ADR-013](docs/ADR/ADR-013-rust-migration.md)), the unprivileged desktop UI is written in **C11 + GTK4**, while the core networking engine (`crates/multiwifi-core`) and privileged system daemon (`crates/multiwifi-daemon`) have been migrated to **idiomatic Rust (Edition 2021)**.

---

## Table of Contents

- [Key Features](#key-features)
- [Performance & Benchmarks](#performance--benchmarks)
- [Operating Modes](#operating-modes)
- [Architecture & Security](#architecture--security)
- [System Requirements](#system-requirements)
- [Installation & Packages](#installation--packages)
- [Building from Source](#building-from-source)
- [Quick Start](#quick-start)
- [D-Bus Automation API](#d-bus-automation-api)
- [Configuration](#configuration)
- [Testing & Quality Assurance](#testing--quality-assurance)
- [Documentation Index](#documentation-index)
- [License](#license)

---

## Key Features

- **Dual Multi-Path Engines:**
  - **Load Balancing Mode:** Per-flow affinity balancing via declarative `nftables` connection marking (`ct mark` in `table inet multiwifi` + MASQUERADE in `table ip multiwifi`), policy routing (`table 10001+`, rule priorities `20001+`/`21001+`), and dynamic Weighted Round-Robin (WRR) scheduling. No remote aggregation server required.
  - **MPTCP Tunnel Bonding Mode:** Single-stream packet bonding over Linux Multipath TCP and virtual TUN encapsulation (`mwtun0`, MTU `1420`) through an aggregation relay server.
- **Hybrid C11/GTK4 + Rust Architecture:**
  - Unprivileged C11/GTK4 desktop UI (`multiwifi-gui`) with asynchronous task lifetime tracking (`active_async_tasks`), compact status chips, Wi-Fi network scanner, and live link metrics.
  - Pure-Rust privileged system daemon (`multiwifi-daemon`, `Tokio` + `zbus`) with `#![deny(unsafe_code)]`, ~`9.3–9.9 MB` live RSS, and zero runtime linkage against GLib or `libnm`.
- **Native DNS Management Subsystem:**
  - Built-in DNS configuration dialog supporting Cloudflare, Google Public DNS, Quad9, AdGuard presets and custom IPv4/IPv6 resolvers.
  - Hardened atomic swap of `/etc/resolv.conf` with symlink traversal defense (`O_NOFOLLOW` / `lstat`), NetworkManager coordination (`/etc/NetworkManager/conf.d/99-multiwifi-dns.conf`), and automated restoration backup (`/run/multiwifi/resolv.conf.bak`).
- **Strict Privilege Separation & Hardened Systemd Service:**
  - The GTK4 GUI runs 100% unprivileged in user space.
  - Mutating operations (routing tables, `nftables`, TUN, DNS) are isolated in `multiwifi-daemon` and gated asynchronously by Polkit (`io.github.multiwifi.modify`) over System D-Bus (`io.github.multiwifi.Daemon`). Read-only queries are permitted without privilege escalation (`RunDiagnostics` rate-limited at `1.5 s`).
  - Sandboxed systemd service: `ProtectSystem=strict`, `ProtectHome=true`, `ReadWritePaths=/etc /run`, `RuntimeDirectory=multiwifi`, and `CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW`.
- **Proactive Path Quality Monitoring with Multi-Port Fallback:**
  - Continuous non-blocking health checks measuring real-time RTT, jitter, and rolling packet loss.
  - Multi-port socket fallback probe (`53` $\rightarrow$ `80` $\rightarrow$ `443`, `250 ms` per port) avoiding false-negative offline states on strict home routers.
  - GUI stops duplicate local probes and consumes D-Bus `PathChanged` signals when connected to `multiwifi-daemon`.
- **Transactional Routing with Atomic Rollback:**
  - All Netlink route and `ip rule` mutations are tracked in disk-backed INI journals (`/run/multiwifi/journals/*.journal`) with readback verification, atomic rollback on failure, and crash recovery reconciliation on daemon startup.
- **Cross-Distribution Packaging:**
  - Build manifests for Arch Linux (`PKGBUILD`), Debian/Ubuntu (`debian/control`), Fedora/RHEL (`packaging/rpm/multiwifi.spec`), CPack (`TGZ` / `.deb`), and portable GUI staging (`packaging/appimage/build-appimage.sh`).
  - Unified lifecycle management script `setup.sh` (`install`, `install-prebuilt`, `uninstall`, `status`, `restart`).

---

## Performance & Benchmarks

MultiWiFi was benchmarked on bare-metal hardware with dual active physical Wi-Fi adapters (PCIe Intel AX211 on Viettel + USB Realtek RTL8188GU on FPT Telecom) on an Intel Core i7-13700H workstation with 32 GB RAM. Full methodology, toolchain breakdown, and legacy C vs Rust hybrid metrics are documented in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| Metric | Single Wi-Fi (Stock Linux) | MultiWiFi Dual-Link | Verified Result / Finding |
| :--- | :--- | :--- | :--- |
| **Download Throughput** | 21.20 Mbps (Viettel) / 4.92 Mbps (FPT) | **19.88 Mbps** (Concurrent Aggregated) | Simultaneous multi-provider stream distribution |
| **DNS / Gaming Latency** | 31.94 ms avg (Viettel) | **29.41 ms avg** (FPT Steering) | **-2.53 ms lower latency, 4x less jitter (0.85 ms vs 3.67 ms)** |
| **Packet Loss & Spike Immunity** | 87.6 ms avg, **20% loss, 244 ms max** (Viettel) | **25.80 ms avg, 0% loss, 0.78 ms jitter** (FPT) | Diverts latency-sensitive flows away from degraded path |
| **Scheduler Microbenchmark (C)** | N/A | **17.40M flows/s (`57.47 ns/decision`)** | Verified in `tests/test_load_balancer.c` (`-O2`) |
| **Bonding Parser (C)** | N/A | **1,465 MB/s (~11.72 Gbps)** | Verified in `tests/test_bonding.c` (`-O2`) |
| **D-Bus IPC Roundtrip (C)** | N/A | **437 µs** | Verified in `tests/test_dbus_daemon.c` |
| **Rust Daemon Footprint** | N/A | **9.30–9.86 MB RSS**, **8.23 MB** bin (`6.20 MB` stripped) | Zero GLib/libnm runtime dependency (`multiwifi-daemon`) |
| **Test Coverage** | N/A | **48 Rust tests + 10 CTest suites (100% pass)** | Includes live C `GDBus` $\leftrightarrow$ Rust daemon parity & ASan checks |

---

## Operating Modes

```text
               +-------------------------------------------+
               |        Concurrent Wi-Fi Interfaces        |
               |     [wlp0s20f3]         [wlp0s20f0u1]     |
               +-------------------------------------------+
                                     |
               +---------------------+---------------------+
               |                                           |
               v                                           v
   +-----------------------+                   +-----------------------+
   |  Mode 1: Load Balance |                   |    Mode 2: Bonding    |
   |-----------------------|                   |-----------------------|
   | - nftables flow mark  |                   | - MPTCP transport     |
   | - Conntrack affinity  |                   | - TUN encapsulation   |
   | - Policy routing tbls |                   | - Frame multiplexing  |
   | - Dynamic WRR weights |                   | - Anti-recursion host |
   | - No remote server req|                   | - Requires agg server |
   +-----------------------+                   +-----------------------+
               |                                           |
               v                                           v
    Multiple Internet Flows                     Single Aggregated Pipe
    (Downloads, Browsing, P2P)                  (Combined Single Stream)
```

### 1. Multi-Path Load Balancing
- Distributes new TCP/UDP connections across active Wi-Fi adapters using Weighted Round-Robin (WRR) with GCD scaling.
- Preserves flow affinity via connection marks (`ct mark` in `table inet multiwifi` with NAT masquerade in `table ip multiwifi`), preventing session resets on HTTPS, banking, and TLS handshakes.
- Routes marked packets through per-uplink policy tables (`10001+`) and priorities (`20001+` source IP, `21001+` fwmark) via `rtnetlink`.
- Dynamically scales path weights based on real-time link latency and rolling loss rates, and restores routing cleanly when stopped.

### 2. MPTCP WAN Bonding
- Combines physical links into a single virtual tunnel adapter (`mwtun0`, MTU `1420`).
- Encapsulates Layer 3 IP packets inside a 16-byte framed streaming protocol over Multipath TCP.
- Features streaming byte resynchronization, magic header validation (`0x4d574631` / `"MWF1"`), ephemeral session HMAC authentication, and anti-spoofing filtering dropping forged loopback source packets.
- Automatically installs dedicated host route bypasses (`LoopPrevention`) so tunnel control traffic never recursively routes through `mwtun0`.

---

## Architecture & Security

```text
   +-------------------------------------------------------------+
   |                       User Session                          |
   |                                                             |
   |   +-----------------------+       +---------------------+   |
   |   |   multiwifi-gui       |       |   CLI / gdbus       |   |
   |   | (C11+GTK4, unpriv.)   |       |   (Scripting)       |   |
   |   +-----------------------+       +---------------------+   |
   +-------------------------------|-----------------------------+
                                   | D-Bus System Bus
                                   | (io.github.multiwifi.Daemon / Daemon1)
   +-------------------------------|-----------------------------+
   |                               v                             |
   |              Async Polkit Authorization (zbus)              |
   |                 (io.github.multiwifi.modify)                |
   |                               |                             |
   |                               v                             |
   |   +-----------------------------------------------------+   |
   |   |           multiwifi-daemon (Rust 2021)              |   |
   |   |  #![deny(unsafe_code)], Tokio runtime, sandboxed    |   |
   |   +-----------------------------------------------------+   |
   |         |              |            |            |          |
   |         v              v            v            v          |
   |   +-----------+ +-------------+ +--------+ +------------+   |
   |   | rtnetlink | | libnftables | |  TUN   | |    DNS     |   |
   |   |(IP Rules) | |(Flow Marks) | |(mwtun0)| |(O_NOFOLLOW)|   |
   |   +-----------+ +-------------+ +--------+ +------------+   |
   +-------------------------------------------------------------+
```

---

## System Requirements

- **Operating System:** Linux Kernel 5.15+ (Linux 6.x / 7.x recommended for MPTCP).
- **Build Toolchain:**
  - C11 compiler (GCC or Clang) & CMake ($\ge$ 3.16)
  - Rust toolchain (`rustc` + `cargo`, Edition 2021) for building `multiwifi-daemon` and `multiwifi-core`
  - `pkg-config`
- **Libraries & Runtime Dependencies:**
  - `glib-2.0` / `gio-2.0` / `gio-unix-2.0` ($\ge$ 2.74) — used by `multiwifi-gui` and C test suites
  - `gtk4` ($\ge$ 4.10) — used by `multiwifi-gui`
  - `libnm` ($\ge$ 1.40) + running `NetworkManager` — GUI Wi-Fi scanning; Rust daemon queries `NetworkManager` directly over System D-Bus with `/proc/net/route` fallback
  - `libnftables` ($\ge$ 1.0.2) — `nftables` transaction execution in `multiwifi-core` / `multiwifi-daemon`
  - `polkit` (`polkitd` runtime + `polkit-gobject-1` for CMake fallback/tests)
- **Hardware:** At least two network interfaces (e.g., internal PCIe Wi-Fi + USB Wi-Fi dongle or USB tethering).

---

## Installation & Packages

### Arch Linux
```bash
cd packaging/arch
makepkg -si
```

### Debian / Ubuntu
```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../multiwifi_*.deb
```

### Fedora / RHEL
```bash
rpmbuild -ba packaging/rpm/multiwifi.spec
sudo dnf install ~/rpmbuild/RPMS/x86_64/multiwifi-*.rpm
```

### Portable GUI AppDir / AppImage
```bash
./packaging/appimage/build-appimage.sh
./build/AppDir/AppRun
```
*(The AppDir stages only unprivileged GUI components; `multiwifi-daemon` must remain installed natively on the host as a privileged system service.)*

---

## Building from Source

```bash
# 1. Clone repository
git clone https://github.com/TrVHau/multi-wifi.git
cd multi-wifi

# 2. Lint and test Rust workspace crates (multiwifi-core + multiwifi-daemon)
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace

# 3. Configure & build C11 GUI + Rust daemon (via CMake MULTIWIFI_RUST_DAEMON=ON)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j$(nproc)

# 4. Run CTest verification suites
ctest --test-dir build --output-on-failure
```

*(Pass `-DMULTIWIFI_RUST_DAEMON=OFF` to build the legacy C daemon fallback instead of the Rust daemon, e.g. for C ASan/UBSan builds.)*

---

## Quick Start

### 1. System Installation & Lifecycle (`setup.sh`)
`setup.sh` builds as your regular user (preserving `~/.cargo/bin`) and elevates only for system installation and `systemd` registration:

```bash
# Build cleanly and install binaries, systemd service, D-Bus & Polkit rules
./setup.sh install   # Or: ./setup.sh

# Check system daemon status
sudo ./setup.sh status

# Restart daemon after configuration changes
sudo ./setup.sh restart

# Clean uninstallation (removes installed files and disables service)
sudo ./setup.sh uninstall
```

### 2. Launch Desktop GUI
```bash
# Launch the unprivileged GTK4 user interface
multiwifi-gui
```

### 3. Local Session Testing (Daemon Binary)
```bash
# Inspect daemon version and API level
./build/multiwifi-daemon --version

# Run in user session bus without Polkit for unprivileged local diagnostics
./build/multiwifi-daemon --session --no-polkit
```

---

## D-Bus Automation API

Daemon operations can be scripted via System D-Bus (`io.github.multiwifi.Daemon` / `io.github.multiwifi.Daemon1`, see [docs/D-BUS-API.md](docs/D-BUS-API.md)):

### Query Status & Capabilities
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.GetStatus

gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.GetCapabilities
```

### Start & Stop Load Balancing
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.StartLoadBalancing "{}"

# Atomic rollback of routes, rules, and nftables tables
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.StopLoadBalancing
```

### Query & Configure DNS
```bash
# Get active DNS configuration
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.GetDNS

# Set Cloudflare DNS
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.SetDNS "['1.1.1.1', '1.0.0.1']"

# Reset DNS back to DHCP default
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.ResetDNS
```

---

## Configuration

Settings can be configured in `/etc/multiwifi/multiwifi.conf` (strict TOML validated with `deny_unknown_fields`; plaintext secrets such as tokens or passwords are rejected — see [docs/CONFIGURATION.md](docs/CONFIGURATION.md)):

```toml
# MultiWiFi Configuration File
schema_version = 1
mode = "load_balance"

[load_balance]
strategy = "weighted_round_robin"
health_check_interval_ms = 2000
failure_threshold = 3
recovery_threshold = 3
cooldown_ms = 5000

[[uplinks]]
device_id = "wlp0s20f3"
enabled = true
weight = 100

[[uplinks]]
device_id = "wlp0s20f0u1"
enabled = true
weight = 80

[bonding]
enabled = false
profile_id = "primary-vpn"
```

---

## Testing & Quality Assurance

MultiWiFi enforces two automated verification layers covering happy paths, symlink hardening, D-Bus authorization, and isolated network-namespace netlink/nftables transactions:

### 1. Rust Workspace (`cargo test --workspace` — 48 tests)

| Crate | Tests | Focus Area |
| :--- | :--- | :--- |
| `multiwifi-core` | 38 passed | TOML parser & secret rejection, capability probing, WRR scheduler + concurrency stress, `nftables` dual-table builders & netns lifecycle, INI journals, Netlink rule/route transactions, `path_monitor`, `tun_device`, `loop_prevention`, `bonding_protocol` & `bonding_client` |
| `multiwifi-daemon` | 10 passed | `/proc/net/route` & NM state mapping, atomic DNS symlink hardening, operation history pruning, runtime `multiwifi.conf` loading & secret rejection, `zbus` D-Bus IPC roundtrip, signal emission & session Polkit denial |

### 2. CMake / CTest Suites (`ctest --test-dir build` — 10 suites)

| Test Suite | Focus Area | Verification Strategy |
| :--- | :--- | :--- |
| `test_config` | Parser & schema safety | TOML parsing, range validation, secret rejection |
| `test_capabilities` | Kernel capabilities | Safe probing, secret redaction filters |
| `test_wifi_device` | `libnm` inventory & mapping | Interface state transitions, lifecycle management |
| `test_path_monitor` | Link quality & RTT | Socket probe fallback (`53 -> 80 -> 443`), jitter, hysteresis |
| `test_dbus_daemon` | Daemon IPC & Polkit | Lifecycle state machine, DNS methods, bounded queue |
| `test_routing_manager` | Transactional routing | Isolated netns, journal persistence, atomic rollback |
| `test_load_balancer` | WRR & `nftables` | Concurrency safety, GCD normalization, dual-table teardown |
| `test_bonding` | MPTCP & TUN aggregation | Frame parsing (`0x4d574631`), garbage recovery, anti-spoofing |
| `test_packaging` | Distribution safety | FHS permissions, systemd safety, AppDir isolation |
| `test_rust_parity` | C `GDBus` $\leftrightarrow$ Rust Daemon | Live isolated `dbus-daemon` XML introspection match, IPC contract, signals, DNS `O_NOFOLLOW`, Polkit enforcement & clean exit |

Run all checks:
```bash
cargo fmt --all -- --check && cargo clippy --workspace --all-targets -- -D warnings && cargo test --workspace && ctest --test-dir build --output-on-failure
```

---

## Documentation Index

- [docs/ADR/ADR-013-rust-migration.md](docs/ADR/ADR-013-rust-migration.md) — Strangler Fig migration plan (Rust core & daemon + C11/GTK4 GUI).
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — Empirical benchmark report, hardware measurements, and Rust/C footprint data.
- [docs/NETWORK_DESIGN.md](docs/NETWORK_DESIGN.md) — Dataplane architecture, routing policies, and `nftables` rules.
- [docs/SECURITY.md](docs/SECURITY.md) — Threat model, privilege separation, Polkit authorization, and security mitigations.
- [docs/D-BUS-API.md](docs/D-BUS-API.md) — Complete D-Bus interface (`io.github.multiwifi.Daemon1`) and signal specifications.
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — Configuration file schema and validation rules.
- [docs/TESTING.md](docs/TESTING.md) — Rust & CTest testing strategy, namespace isolation, and sanitizer verification.
- [docs/PACKAGING.md](docs/PACKAGING.md) — Distribution packaging standards (`PKGBUILD`, `.deb`, `.spec`, CPack, AppDir) and release checklist.
- [docs/IMPLEMENTATION_ROADMAP.md](docs/IMPLEMENTATION_ROADMAP.md) — Complete milestone, phase, and Rust migration report.
- [docs/ADR/](docs/ADR/) — Architecture Decision Records (`ADR-001` through `ADR-013`).

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
