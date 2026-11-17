<p align="center">
  <img src="data/icons/hicolor/512x512/apps/io.github.multiwifi.MultiWiFi.png" width="140" alt="MultiWiFi Logo" />
</p>

# MultiWiFi

[![CI](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml/badge.svg)](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/Language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![GTK4](https://img.shields.io/badge/GUI-GTK4%20%7C%20Libadwaita%20Style-green.svg)](https://www.gtk.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-orange.svg)](https://kernel.org/)
[![Security](https://img.shields.io/badge/Security-Polkit%20%2B%20D--Bus-red.svg)](docs/SECURITY.md)

**MultiWiFi** is a high-performance native Linux desktop application and privileged system daemon written in modern C11. It aggregates and balances network traffic across multiple concurrent Wi-Fi adapters (such as internal PCIe Wi-Fi, USB Wi-Fi dongles, or tethered mobile 5G hotspots) to maximize internet bandwidth, minimize latency, and provide automatic zero-downtime failover.

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
  - **Load Balancing Mode:** Per-flow affinity balancing via declarative `nftables` connection marking (`ct mark`) and dynamic Weighted Round-Robin (WRR) scheduling. No remote aggregation server required.
  - **MPTCP Tunnel Bonding Mode:** Single-stream packet bonding over Linux Multipath TCP and virtual TUN encapsulation (`mw-tun0`, MTU 1420) through an aggregation relay server.
- **Modern Minimalist Desktop GUI:**
  - Polished GNOME-style interface with intuitive toggle switches (`GtkSwitch`), compact status chips, Wi-Fi network scanner, and live link metrics.
  - Asynchronous lifetime safety (`active_async_tasks` tracking) completely eliminating Use-After-Free crashes on dialog closure.
  - Sub-15ms cold start and extremely low memory footprint (~12.5 MB RSS).
- **Native DNS Management Subsystem:**
  - Built-in DNS configuration dialog supporting popular secure presets (Cloudflare, Google Public DNS, Quad9, AdGuard) and custom IPv4/IPv6 resolvers.
  - Hardened atomic swap of `/etc/resolv.conf` with symlink traversal defense (`O_NOFOLLOW` / `lstat`) and automated restoration backup (`/etc/resolv.conf.multiwifi-backup`).
- **Strict Privilege Separation & Hardened Systemd Service:**
  - The GTK4 GUI runs 100% unprivileged in user space.
  - Privileged network mutations (routing tables, netfilter, TUN) are isolated in `multiwifi-daemon` authenticated via Polkit (`io.github.multiwifi.modify`) over System D-Bus.
  - Sandboxed systemd service: `ProtectSystem=strict`, `ProtectHome=true`, `ReadWritePaths=/etc /run`, `RuntimeDirectory=multiwifi`, and `CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW`.
  - Polkit rules allowing seamless authorization for active console session administrators (wheel/sudo).
- **Proactive Path Quality Monitoring with Multi-Port Fallback:**
  - Continuous, non-blocking health checks measuring real-time RTT, jitter, and rolling packet loss.
  - Multi-port socket fallback probe (DNS port 53 $\rightarrow$ HTTP port 80 $\rightarrow$ HTTPS port 443 with 250ms timeouts) eliminating false-negative offline alerts on strict home routers.
  - Dynamic link steering directs latency-sensitive flows to the fastest path and penalizes degraded links.
  - GUI stops local socket probes and synchronizes live metrics via D-Bus `PathChanged` signals when the daemon is active, eliminating duplicate network probes.
- **Transactional Routing with Atomic Rollback:**
  - All route and rule mutations are tracked in disk-backed journals (`/run/multiwifi/journals/*.journal`).
  - Readback verification ensures atomic rollback if any netlink command fails, preventing orphaned routes. Automatic recovery clean-up runs on daemon startup.
- **Non-Destructive Dual-Table Firewall Architecture:**
  - Operates inside isolated nftables tables (`table inet multiwifi_table` for flow marks; `table ip multiwifi_table` for postrouting masquerade) without flushing or conflicting with existing firewall tools (UFW, firewalld, Docker, Tailscale).
  - Unconditional teardown of both tables guarantees zero NAT table leaks.
- **Cross-Distribution Packaging:**
  - Official build manifests for Arch Linux (`PKGBUILD`), Debian/Ubuntu (`.deb`), Fedora/RHEL (`.spec`), and portable standalone AppImages.
  - Unified system management script `setup.sh` (`install`, `uninstall`, `status`, `restart`).

---

## Performance & Benchmarks

MultiWiFi was benchmarked on bare-metal hardware with dual active physical Wi-Fi adapters (PCIe Viettel + USB FPT Telecom). Full details and methodology are documented in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| Metric | Single Wi-Fi (Stock Linux) | MultiWiFi Dual-Link | Performance Impact |
| :--- | :--- | :--- | :--- |
| **Download Throughput** | 39.4 Mbps (1 card) | **47.74 Mbps** (Aggregated) | **+21.0% throughput increase** |
| **DNS / Gaming Latency** | 29.0 ms (Viettel) | **22.5 ms** (FPT Steering) | **-6.5 ms lower latency (-22.4%)** |
| **Scheduler Capacity** | N/A | **17.40 Million flows/s** | Capable of multi-gigabit line-rates |
| **Scheduler Decision Time** | N/A | **57.47 ns / decision** | Imperceptible scheduling latency |
| **Bonding Parser Speed** | N/A | **1,465 MB/s (~11.72 Gbps)** | Exceeds 10G interface throughput |
| **D-Bus IPC Roundtrip** | N/A | **437 µs** | Instantaneous GUI responsiveness |
| **DNS Switching Latency** | N/A | **1.8 ms** (Atomic symlink-safe switch) | Zero DNS downtime |
| **Probe Fallback Time** | N/A | **250 ms / port (DNS 53 -> 80 -> 443)** | Eliminates false link drops |
| **Memory Footprint** | N/A | **~12.0 MB (Daemon) + ~12.5 MB (GUI)** | Minimal system resource usage |

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
- Preserves flow affinity via connection marks (`ct mark`), preventing session resets on HTTPS, banking, and TLS handshakes.
- Dynamically scales path weights based on real-time link latency and rolling loss rates.
- Automatically restores original default routing when stopped.

### 2. MPTCP WAN Bonding
- Combines physical links into a single virtual tunnel adapter (`mw-tun0`, MTU 1420).
- Encapsulates Layer 3 IP packets inside a lightweight framed streaming protocol over Multipath TCP.
- Features automatic byte synchronization, magic header validation (`0x4D574246`), and token-based HMAC authentication.
- Automatically installs dedicated host route bypasses to ensure tunnel traffic never recursively routes through itself.
- Features anti-spoofing protection dropping forged loopback packets.

---

## Architecture & Security

```text
   +-------------------------------------------------------------+
   |                       User Session                          |
   |                                                             |
   |   +-----------------------+       +---------------------+   |
   |   |   multiwifi-gui       |       |   CLI / busctl      |   |
   |   |   (GTK4, unprivileged)|       |   (Scripting)       |   |
   |   +-----------------------+       +---------------------+   |
   +-------------------------------|-----------------------------+
                                   | D-Bus System Bus
                                   | (io.github.multiwifi.Daemon)
   +-------------------------------|-----------------------------+
   |                               v                             |
   |                     Polkit Authorization                    |
   |                 (io.github.multiwifi.modify)                |
   |                               |                             |
   |                               v                             |
   |   +-----------------------------------------------------+   |
   |   |                  multiwifi-daemon                   |   |
   |   |     (Privileged System Service, Sandboxed Root)     |   |
   |   +-----------------------------------------------------+   |
   |          |               |               |             |    |
   |          v               v               v             v    |
   |   +-------------+ +---------------+ +---------+ +--------+  |
   |   |  rtnetlink  | |   nftables    | |   TUN   | |  DNS   |  |
   |   | (IP Rules)  | | (Flow Marks)  | | (MPTCP) | |(resolv)|  |
   |   +-------------+ +---------------+ +---------+ +--------+  |
   +-------------------------------------------------------------+
```

---

## System Requirements

- **Operating System:** Linux Kernel 5.15+ (Linux 6.x / 7.x recommended for MPTCP).
- **Core Libraries:**
  - GLib / GIO / GIO-Unix $\ge$ 2.74
  - GTK4 $\ge$ 4.10
  - libnm (NetworkManager) $\ge$ 1.40
  - polkit-gobject-1
  - libnftables $\ge$ 1.0.2
- **Hardware:** At least two network interfaces (e.g., internal Wi-Fi + USB Wi-Fi adapter or USB tethering).

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

### Standalone AppImage
```bash
./packaging/appimage/build_appimage.sh
./build-appimage/MultiWiFi-x86_64.AppImage
```

---

## Building from Source

```bash
# 1. Clone repository
git clone https://github.com/TrVHau/multi-wifi.git
cd multi-wifi

# 2. Configure with strict warnings (mandated C11)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# 3. Build daemon, GUI, and core libraries
cmake --build build -j$(nproc)

# 4. Run automated test suites (100% pass required)
ctest --test-dir build --output-on-failure
```

---

## Quick Start

### 1. One-Click Installation & Management
You can easily manage the entire MultiWiFi lifecycle using the unified `setup.sh` script:

```bash
# Automated system installation (builds, installs systemd service, registers D-Bus & Polkit)
sudo ./setup.sh install   # Or simply: sudo ./setup.sh

# Check system daemon status
sudo ./setup.sh status

# Restart daemon after configuration changes
sudo ./setup.sh restart

# Complete and clean uninstallation (removes all files and disables service)
sudo ./setup.sh uninstall
```

### 2. Launch Desktop GUI
```bash
# Launch the modern GTK4 user interface (runs unprivileged)
multiwifi-gui
```

---

## D-Bus Automation API

All daemon operations can be scripted via D-Bus without requiring GUI access:

### Query Status
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.GetStatus
```

### Start Load Balancing
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StartLoadBalancing "{}"
```

### Query & Configure DNS
```bash
# Get active DNS configuration
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.GetDNS

# Set Cloudflare DNS
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.SetDNS "['1.1.1.1', '1.0.0.1']"

# Reset DNS back to DHCP default
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.ResetDNS
```

### Stop Load Balancing (Atomic Rollback)
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StopLoadBalancing
```

---

## Configuration

Settings can be configured in `/etc/multiwifi/multiwifi.conf` (TOML format):

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
server_host = "198.51.100.1"
server_port = 5000
auth_token = "secret-token-redacted"
mtu = 1420
```

---

## Testing & Quality Assurance

MultiWiFi includes 9 automated CTest suites covering both happy-path scenarios and failure injection:

| Test Suite | Focus Area | Verification Strategy |
| :--- | :--- | :--- |
| `test_config` | Parser & schema safety | TOML parsing, range validation, quotation sanitization |
| `test_capabilities` | Kernel capabilities | Safe probing, secret redaction filters |
| `test_wifi_device` | libnm inventory & mapping | Interface state transitions, lifecycle management |
| `test_path_monitor` | Link quality & RTT | Socket probe fallback (53 -> 80 -> 443), jitter, hysteresis |
| `test_dbus_daemon` | Daemon IPC & Polkit | Lifecycle state machine, DNS methods, bounded queue |
| `test_routing_manager`| Transactional routing | Isolated netns, journal persistence, atomic rollback |
| `test_load_balancer` | WRR & nftables | Concurrency safety, GCD normalization, dual-table teardown |
| `test_bonding` | MPTCP & TUN aggregation | Frame parsing, garbage recovery, anti-spoofing drop |
| `test_packaging` | Distribution safety | FHS permissions, systemd safety, AppImage isolation |

Run all tests:
```bash
ctest --test-dir build --output-on-failure
```

---

## Documentation Index

- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — Comprehensive empirical benchmark report & methodology.
- [docs/NETWORK_DESIGN.md](docs/NETWORK_DESIGN.md) — Dataplane architecture, routing policies, and nftables rules.
- [docs/SECURITY.md](docs/SECURITY.md) — Threat model, privilege separation, Polkit authorization, and security mitigations.
- [docs/D-BUS-API.md](docs/D-BUS-API.md) — Complete D-Bus interface and signal specifications.
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — Configuration file schema and parameters.
- [docs/TESTING.md](docs/TESTING.md) — Testing strategy, namespace isolation, and sanitizer verification.
- [docs/PACKAGING.md](docs/PACKAGING.md) — Distribution packaging standards and release checklist.
- [docs/IMPLEMENTATION_ROADMAP.md](docs/IMPLEMENTATION_ROADMAP.md) — Complete milestone and phase implementation report.
- [docs/ADR/](docs/ADR/) — Architecture Decision Records (ADR-001 through ADR-012).

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
