# MultiWiFi

[![CI](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml/badge.svg)](https://github.com/TrVHau/multi-wifi/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![GTK4](https://img.shields.io/badge/GUI-GTK4%20%7C%20GLib-green.svg)](https://www.gtk.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux-orange.svg)](https://kernel.org/)
[![Security](https://img.shields.io/badge/Privilege-Polkit%20%2B%20D--Bus-red.svg)](docs/SECURITY.md)

MultiWiFi is a high-performance, native Linux desktop application and system daemon written in modern C11. It aggregates and balances network traffic across multiple concurrent Wi-Fi connections (such as built-in Wi-Fi plus USB Wi-Fi dongles or tethered mobile 5G hotspots) to enhance network throughput, decrease latency, and provide automatic failover resilience.

---

## Table of Contents

- [Key Features](#key-features)
- [Operating Modes](#operating-modes)
- [Architecture & Security](#architecture--security)
- [System Requirements](#system-requirements)
- [Installation & Packaging](#installation--packaging)
- [Building from Source](#building-from-source)
- [Quick Start](#quick-start)
- [D-Bus IPC & Automation](#d-bus-ipc--automation)
- [Configuration](#configuration)
- [Testing & Verification](#testing--verification)
- [Implementation Roadmap](#implementation-roadmap)
- [Documentation Index](#documentation-index)
- [License](#license)

---

## Key Features

- **Strict Privilege Separation:** Unprivileged GTK4 GUI runs under standard user accounts. Privileged networking modifications are isolated inside a dedicated system daemon authenticated via Polkit and D-Bus.
- **Transactional Routing with Atomic Rollback:** Route table and rule alterations are recorded in a persistent journal (`/var/run/multiwifi/*.journal`). If any netlink operation fails, transactions atomically roll back to leave system routes clean.
- **Zero Global Firewall Flushing:** Declarative `nftables` dataplane operates inside an isolated table (`table ip multiwifi`) without altering or flushing existing firewall chains (UFW, firewalld, Docker, Tailscale).
- **Dual Operating Modes:**
  - **Load Balancing:** Per-flow affinity via `nftables` + `conntrack` with dynamic Weighted Round-Robin (WRR) scheduling and automatic health degradation penalties.
  - **Bonding:** True multi-path internet aggregation via Linux MPTCP (Multipath TCP) and TUN encapsulation to a remote aggregation server.
- **Dynamic Uplink Quality Monitoring:** Continuous, non-blocking reachability probing with RTT/jitter estimation, rolling packet-loss window, and hysteresis recovery streaks.
- **Cross-Distribution Packaging:** Official support for Arch Linux (`PKGBUILD`), Debian/Ubuntu (`.deb`), Red Hat/Fedora (`.spec`), and portable standalone GUI AppImages.

---

## Operating Modes

```text
               +-------------------------------------------+
               |        Concurrent Wi-Fi Interfaces        |
               |          [wlan0]         [wlan1]          |
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
    (Downloads, Browsing)                       (Combined Throughput)
```

### 1. Multi-Path Load Balancing
- Distributes new TCP/UDP flows across active Wi-Fi adapters using Weighted Round-Robin (WRR) with GCD scaling.
- Preserves flow affinity via connection marks (`ct mark`), preventing TCP session resets and TLS handshake breakages.
- Dynamically scales path weights according to link latency and rolling loss rates.
- Automatically restores original default routing when disabled.

### 2. MPTCP WAN Bonding
- Combines physical links into a single virtual tunnel adapter (`mw-tun0`).
- Encapsulates Layer 3 IP packets inside a lightweight framed streaming protocol over Multipath TCP.
- Features automatic byte synchronization, magic header validation (`0x4D574246`), and token-based HMAC authentication.
- Automatically installs dedicated host route bypasses to ensure aggregation tunnel traffic never recursively routes through itself.

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
   |                   (io.github.multiwifi.modify)              |
   |                                                             |
   |                 multiwifi-daemon (systemd)                  |
   |                                                             |
   |   +-----------------+   +------------------+   +--------+   |
   |   | Routing Manager |   | Load Balancer    |   | Bonding|   |
   |   | - Transactional |   | - WRR Engine     |   | Protocol   |
   |   | - Disk Journal  |   | - Dynamic Health |   | - MPTCP|   |
   |   | - Atomic Rewind |   | - nftables table |   | - TUN  |   |
   |   +-----------------+   +------------------+   +--------+   |
   |                                                             |
   |   +-----------------------------------------------------+   |
   |   | Hardware Inventory & Path Monitor (libnm, netlink)  |   |
   |   +-----------------------------------------------------+   |
   +-------------------------------|-----------------------------+
                                   | rtnetlink / nftables / MPTCP
                                   v
   +-------------------------------------------------------------+
   |                      Linux Kernel                           |
   |    [rtnetlink]    [conntrack/nft]    [MPTCP]    [TUN driver]|
   +-------------------------------------------------------------+
```

### Security Guarantees
1. **Zero Shell Interpolation:** Network commands execute directly through native C library APIs (`rtnetlink`, `libnftables`, `libnm`, POSIX socket APIs).
2. **Crash Reconciliation:** On startup, the daemon scans `/var/run/multiwifi/*.journal` and rewinds any interrupted routing state left by power loss or unclean termination.
3. **Redaction of Secrets:** Wi-Fi passwords, authentication tokens, and private identifiers are scrubbed before reaching stdout, syslog, or memory logs.

---

## System Requirements

- **Linux Kernel:** 5.6 or newer (with `CONFIG_MPTCP=y` and `CONFIG_NETFILTER=y` recommended).
- **Core Libraries:**
  - GLib / GIO 2.74+
  - GTK4 4.10+
  - NetworkManager / libnm 1.40+
  - polkit-gobject-1
  - libnftables
- **Hardware:** Two or more functioning Wi-Fi interfaces (e.g., internal PCI Wi-Fi and external USB dongle).

---

## Installation & Packaging

### Arch Linux
Build and install via `PKGBUILD`:
```bash
cd packaging/arch
makepkg -si
```

### Debian / Ubuntu
Build `.deb` package via CPack or CMake:
```bash
cmake -B build -DBUILD_TESTING=OFF
cmake --build build
cd build && cpack -G DEB
sudo dpkg -i multiwifi-*.deb
```

### Red Hat / Fedora / openSUSE
Use the provided RPM spec file:
```bash
rpmbuild -ba packaging/rpm/multiwifi.spec
```

### Portable GUI AppImage
The portable AppImage bundles the unprivileged GTK4 GUI without requiring root rights:
```bash
./packaging/appimage/build-appimage.sh build
# The resulting AppDir is staged under build/AppDir
```

---

## Building from Source

### 1. Install Dependencies

**Debian / Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libglib2.0-dev libgtk-4-dev libnm-dev libpolkit-gobject-1-dev libnftables-dev dbus
```

**Arch Linux:**
```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf glib2 gtk4 libnm polkit nftables
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc cmake ninja-build pkgconf-pkg-config \
    glib2-devel gtk4-devel NetworkManager-libnm-devel polkit-devel libnftables-devel
```

### 2. Compile and Test

```bash
# Clone the repository
git clone https://github.com/TrVHau/multi-wifi.git
cd multi-wifi

# Configure build with testing enabled
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Compile with strict compiler warnings (-Wall -Wextra -Wpedantic)
cmake --build build

# Run complete test suite (9 test suites)
ctest --test-dir build --output-on-failure --verbose
```

### 3. AddressSanitizer & UndefinedBehaviorSanitizer Build
```bash
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

---

## Quick Start

### 1. Start the System Daemon
Enable and start the systemd service:
```bash
sudo systemctl enable --now multiwifi-daemon.service
```

*For local development and testing without installing to `/usr`:*
```bash
# Run on D-Bus session bus with Polkit verification bypassed
./build/multiwifi-daemon --session --no-polkit
```

### 2. Launch the Desktop GUI
```bash
./build/multiwifi-gui
```
The interface will automatically:
- Discover all Wi-Fi adapters and active SSIDs.
- Display latency (RTT), jitter, and packet loss in real time.
- Provide one-click controls for **Load Balancing** and **Bonding**.

---

## D-Bus IPC & Automation

MultiWiFi provides a full D-Bus API (`io.github.multiwifi.Daemon`) on the system bus for automation and scripting.

### Query System Status
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.GetStatus
```

### Set Custom Path Weight
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.SetPathWeight "wlan0" 250
```

### Start Load Balancing
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StartLoadBalancing "{}"
```

### Stop Load Balancing (Rollback)
```bash
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StopLoadBalancing
```

---

## Configuration

Configuration is loaded from `/etc/multiwifi/multiwifi.conf` or user overrides:

```ini
[general]
mode = load_balancing
auto_failover = true
health_check_interval_ms = 3000
log_level = info

[uplinks]
preferred_device = wlan0

[device "wlan0"]
weight = 100
probe_target = 1.1.1.1

[device "wlan1"]
weight = 100
probe_target = 8.8.8.8

[bonding]
server_host = 198.51.100.1
server_port = 5000
auth_token = <secret-token-redacted>
mtu = 1420
```

---

## Testing & Verification

MultiWiFi is tested with 9 automated CTest suites covering both happy-path and failure-injection scenarios:

| Test Suite | Coverage Area | Verification Techniques |
| :--- | :--- | :--- |
| `test_config` | Parser & schema safety | Keyfile parsing, range validation, secret rejection in ordinary values |
| `test_capabilities` | Kernel capability probing | Safe capability checks, secret redaction filter testing |
| `test_wifi_device` | libnm inventory & mapping | Interface state transitions, device lifecycle management |
| `test_path_monitor` | Network link quality | Non-blocking probing, RTT/jitter estimation, hysteresis streak recovery |
| `test_dbus_daemon` | Daemon IPC & Polkit | Lifecycle state machine, D-Bus methods/signals, Polkit access enforcement |
| `test_routing_manager`| Transactional routing | Isolated Linux network namespaces, journal persistence, atomic rollback |
| `test_load_balancer` | WRR & nftables | Concurrency safety, GCD normalization, health penalty, netns firewall |
| `test_bonding` | MPTCP & TUN aggregation | Frame parsing, garbage recovery, handshake, loop-prevention routing |
| `test_packaging` | Packaging & release safety | FHS permissions, Polkit/systemd safety, AppImage privilege isolation |

Execute all tests in isolated environments:
```bash
ctest --test-dir build --output-on-failure
```

---

## Implementation Roadmap

All planned implementation phases are completed and verified:

- [x] **Phase 0: Foundation** — Clean C11/GTK4 build system, logging with secret redaction, kernel capability inspection, config schema/validation, and CI automation.
- [x] **Phase 1: Wi-Fi Inventory** — Read-only libnm integration; detect and track network adapters, connection states, signal strength, SSID, IPv4/IPv6 addresses, and gateways.
- [x] **Phase 2: Path Monitor** — Independent uplink probing, unprivileged non-blocking reachability tests, RTT/jitter estimation, rolling loss calculation, hysteresis, and recovery streaks.
- [x] **Phase 3: System Daemon** — D-Bus service (`io.github.multiwifi.Daemon`), Polkit authorization (`io.github.multiwifi.modify`), operation lifecycle state machine, and systemd service unit.
- [x] **Phase 4: Routing Transaction Manager** — Plan generation, conflict detection, rtnetlink socket backend, transactional disk journal, readback verification, atomic rollback on failure, and crash-recovery reconciliation in isolated namespaces.
- [x] **Phase 5: Load Balancing** — Weighted Round-Robin (WRR) flow scheduler with GCD scaling, dynamic health penalties on packet loss/RTT, declarative nftables dataplane (`table ip multiwifi`) with flow affinity, zero host firewall flushing, and dynamic D-Bus weighting.
- [x] **Phase 6: Bonding** — Remote aggregation protocol (magic header, framing, authentication handshake, streaming parser with resync), TUN device encapsulation, recursive loop prevention with dedicated host routing, MPTCP transport socket with standard TCP fallback, and bidirectional IP forwarding worker.
- [x] **Phase 7: Packaging & Release** — Native distribution packages for Arch (`PKGBUILD`), Debian (`.deb` with safe `postinst`/`prerm`/`postrm` maintainer scripts), and RPM (`multiwifi.spec` with systemd macros), portable GUI-only AppImage bundle (`AppRun` without privileged escalation), clean lifecycle tests, and security review.

---

## Documentation Index

- [AGENTS.md](AGENTS.md) — Coding standards, rules, and security guidelines.
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
