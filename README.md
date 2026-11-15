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

**MultiWiFi** is a high-performance native Linux desktop application and system daemon written in modern C11. It aggregates and balances network traffic across multiple concurrent Wi-Fi connections (such as internal PCIe Wi-Fi, USB Wi-Fi dongles, or tethered mobile 5G hotspots) to maximize internet bandwidth, minimize latency, and provide automatic zero-downtime failover.

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
  - **Load Balancing Mode:** Per-flow affinity balancing via declarative `nftables` connection marking and dynamic Weighted Round-Robin (WRR) scheduling. No remote server required.
  - **MPTCP Tunnel Bonding Mode:** Single-stream packet bonding over Linux Multipath TCP and TUN encapsulation (`mw-tun0`) through an aggregation relay server.
- **Modern Minimalist Desktop GUI:**
  - Polished GNOME-style card interface with intuitive toggle switches (`GtkSwitch`), compact status chips, and Wi-Fi network scanner.
  - Sub-15ms cold start and extremely low memory footprint (~12.5 MB RSS).
- **Strict Privilege Separation:**
  - The GTK4 GUI runs 100% unprivileged in user space.
  - All privileged network operations (routing table modifications, netfilter rules, TUN devices) are isolated in `multiwifi-daemon` authenticated via Polkit (`io.github.multiwifi.modify`) over D-Bus.
- **Proactive Path Quality Monitoring:**
  - Continuous, non-blocking health checks measuring real-time RTT, jitter, and rolling packet loss.
  - Automatic link steering directs latency-sensitive flows to the fastest path and penalizes degraded links.
- **Transactional Routing with Atomic Rollback:**
  - All route and rule mutations are tracked in disk-backed journals (`/var/run/multiwifi/*.journal`).
  - Readback verification ensures atomic rollback if any netlink command fails, preventing orphaned routes.
- **Non-Destructive Firewall Architecture:**
  - Operates inside an isolated nftables table (`table ip multiwifi`) without flushing or conflicting with existing firewall tools (UFW, firewalld, Docker, Tailscale).
- **Cross-Distribution Packaging:**
  - Official build manifests for Arch Linux (`PKGBUILD`), Debian/Ubuntu (`.deb`), Fedora/RHEL (`.spec`), and portable standalone AppImages.

---

## Performance & Benchmarks

MultiWiFi was benchmarked on bare-metal hardware with dual active Wi-Fi adapters (PCIe Viettel + USB FPT Telecom). Full details and methodology are documented in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| Metric | Single Wi-Fi | MultiWiFi Dual-Link | Performance Impact |
| :--- | :--- | :--- | :--- |
| **Download Throughput** | 39.4 Mbps (1 card) | **47.7 Mbps** (Aggregated) | **+21.0% throughput increase** |
| **DNS / Gaming Latency** | 29.0 ms (Viettel) | **22.5 ms** (FPT Steering) | **-6.5 ms lower latency (-22.4%)** |
| **Scheduler Capacity** | N/A | **17.40 Million flows/s** | Capable of multi-gigabit line-rates |
| **Scheduler Decision Time** | N/A | **57.47 ns / decision** | Imperceptible scheduling latency |
| **Bonding Parser Speed** | N/A | **1,465 MB/s (~11.72 Gbps)** | Exceeds 10G interface throughput |
| **D-Bus IPC Roundtrip** | N/A | **437 µs** | Instantaneous GUI responsiveness |
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
- Combines physical links into a single virtual tunnel adapter (`mw-tun0`).
- Encapsulates Layer 3 IP packets inside a lightweight framed streaming protocol over Multipath TCP.
- Features automatic byte synchronization, magic header validation (`0x4D574246`), and token-based HMAC authentication.
- Automatically installs dedicated host route bypasses to ensure tunnel traffic never recursively routes through itself.

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
   |   |          (Privileged System Service, Root)          |   |
   |   +-----------------------------------------------------+   |
   |          |                      |                      |    |
   |          v                      v                      v    |
   |   +-------------+       +---------------+       +-------+   |
   |   |  rtnetlink  |       |   nftables    |       |  TUN  |   |
   |   | (IP Rules)  |       | (Flow Marks)  |       | (MPTCP)   |
   |   +-------------+       +---------------+       +-------+   |
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

### 1. One-Click Installation & Removal
You can easily install or remove MultiWiFi using the automated scripts:

```bash
# Automated system installation (builds, installs systemd service, registers D-Bus & Polkit)
sudo ./install.sh

# Complete and clean uninstallation (removes all files and disables service)
sudo ./uninstall.sh
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

### Stop Load Balancing (Atomic Rollback)
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StopLoadBalancing
```

---

## Configuration

Default settings can be configured in `/etc/multiwifi/multiwifi.conf`:

```ini
[general]
mode = load_balancing
auto_failover = true
health_check_interval_ms = 3000
log_level = info

[uplinks]
preferred_device = wlp0s20f3

[device "wlp0s20f3"]
weight = 100
probe_target = 1.1.1.1

[device "wlp0s20f0u1"]
weight = 80
probe_target = 8.8.8.8

[bonding]
server_host = 198.51.100.1
server_port = 5000
auth_token = <secret-token-redacted>
mtu = 1420
```

---

## Testing & Quality Assurance

MultiWiFi includes 9 automated CTest suites covering both happy-path scenarios and failure injection:

| Test Suite | Focus Area | Verification Strategy |
| :--- | :--- | :--- |
| `test_config` | Parser & schema safety | Keyfile parsing, range validation, secret rejection |
| `test_capabilities` | Kernel capabilities | Safe probing, secret redaction filters |
| `test_wifi_device` | libnm inventory & mapping | Interface state transitions, lifecycle management |
| `test_path_monitor` | Link quality & RTT | Non-blocking probing, jitter estimation, hysteresis |
| `test_dbus_daemon` | Daemon IPC & Polkit | Lifecycle state machine, Polkit access enforcement |
| `test_routing_manager`| Transactional routing | Isolated netns, journal persistence, atomic rollback |
| `test_load_balancer` | WRR & nftables | Concurrency safety, GCD normalization, netns firewall |
| `test_bonding` | MPTCP & TUN aggregation | Frame parsing, garbage recovery, loop-prevention |
| `test_packaging` | Distribution safety | FHS permissions, systemd safety, AppImage isolation |

Run all tests:
```bash
ctest --test-dir build --output-on-failure
```

---

## Documentation Index

- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — Comprehensive empirical benchmark report & methodology.
- [docs/NETWORK_DESIGN.md](docs/NETWORK_DESIGN.md) — Dataplane architecture, routing policies, and nftables rules.
- [docs/SECURITY.md](docs/SECURITY.md) — Threat model, privilege separation, and Polkit authorization.
- [docs/D-BUS-API.md](docs/D-BUS-API.md) — Complete D-Bus interface and signal specifications.
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — Configuration file schema and parameters.
- [docs/TESTING.md](docs/TESTING.md) — Testing strategy, namespace isolation, and fixtures.
- [docs/PACKAGING.md](docs/PACKAGING.md) — Distribution packaging standards and release checklist.
- [docs/ADR/](docs/ADR/) — Architecture Decision Records.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
