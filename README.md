# MultiWiFi

Native Linux desktop application for managing multiple Wi-Fi uplinks in two distinct modes:

- **Load balancing:** distribute new flows across uplinks while preserving per-flow affinity.
- **Bonding:** aggregate paths through a remote endpoint using MPTCP. This requires a compatible server; two Wi-Fi connections alone do not create Internet-wide aggregation.

> This is an architecture and implementation specification. It does not claim that the application is implemented or that networking behavior has been verified.

## Architecture

```text
GTK4 GUI (unprivileged)
   | D-Bus + polkit
   v
MultiWiFi system daemon
   +-- NetworkManager/libnm
   +-- rtnetlink
   +-- nftables + conntrack
   +-- MPTCP + TUN (bonding)
   |
   v
Linux kernel networking

Bonding additionally requires an authenticated remote aggregation server.
```

## Technology

C11, GTK4, GLib/GIO, libnm, NetworkManager, D-Bus, polkit, rtnetlink, nftables, conntrack, Linux MPTCP, TUN, CMake or Meson.

## Documentation

- `AGENTS.md`: coding-agent rules
- `docs/NETWORK_DESIGN.md`: dataplane and network behavior
- `docs/SECURITY.md`: threat model and privilege separation
- `docs/D-BUS-API.md`: GUI/daemon API
- `docs/CONFIGURATION.md`: configuration and runtime state
- `docs/TESTING.md`: test strategy
- `docs/PACKAGING.md`: distribution packages
- `docs/IMPLEMENTATION_ROADMAP.md`: implementation phases
- `docs/ADR/`: architecture decisions
- `prompts/PHASE-0.md`: first coding-agent prompt

## Development and Build

### Prerequisites
- C11 compliant compiler (GCC 11+ or Clang 13+)
- CMake 3.20+
- `pkg-config`
- GLib / GIO 2.74+ (`libglib2.0-dev` on Debian/Ubuntu, `glib2` on Arch)
- GTK4 4.10+ (`libgtk-4-dev` on Debian/Ubuntu, `gtk4` on Arch)

### Build Commands

```bash
# Configure build with testing enabled
cmake -B build -DBUILD_TESTING=ON

# Compile all targets with warnings enabled (-Wall -Wextra -Wpedantic)
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure --verbose
```

### Running Components

```bash
# Run unprivileged GTK4 GUI
./build/multiwifi-gui

# Run system daemon on session bus for testing/development (no root/polkit required)
./build/multiwifi-daemon --session --no-polkit

# Run system daemon in production (requires system bus permissions and polkit)
sudo ./build/multiwifi-daemon
```

## Phases

- [x] **Phase 0:** Foundation — Clean C11/GTK4 build system, logging with secret redaction, capability inspection, config schema/validation, unit tests and CI.
- [x] **Phase 1:** Wi-Fi inventory — Read-only libnm integration; detect and list adapters, connection states, signal strength, SSID, IPv4/IPv6 addresses and gateways.
- [x] **Phase 2:** Path monitor — Independent uplink probing, unprivileged non-blocking reachability tests, RTT/jitter estimation, rolling loss calculation, hysteresis and recovery streaks.
- [x] **Phase 3:** System daemon — D-Bus service (`io.github.multiwifi.Daemon`), polkit action (`io.github.multiwifi.modify`), operation lifecycle state machine (`QUEUED` -> `VALIDATING` -> `APPLYING` -> `VERIFYING` -> `SUCCEEDED` / `ROLLING_BACK` -> `FAILED`), IPC methods/signals, and systemd service unit.
- [x] **Phase 4:** Routing transaction manager — Plan generation, conflict detection, rtnetlink socket backend, transactional journal on disk, readback verification, atomic rollback on failure, crash-recovery reconciliation, and isolated network namespace integration tests.
- [x] **Phase 5:** Load balancing — Weighted Round-Robin (WRR) flow scheduler with GCD scaling, dynamic health penalties on packet loss/RTT, declarative nftables dataplane (`table ip multiwifi`) with source-port flow affinity, zero host firewall flushing, atomic teardown/rollback, and dynamic D-Bus path weighting.
- [x] **Phase 6:** Bonding — Remote aggregation protocol (magic header, framing, authentication handshake, streaming parser with resync), TUN device encapsulation, recursive loop prevention with dedicated host routing, MPTCP transport socket with standard TCP fallback, bidirectional IP forwarding worker, and transactional lifecycle management.
- [ ] **Phase 7:** Packaging and release.

## Constraints

Two independent Wi-Fi paths generally require two adapters. If both links share one constrained WAN, throughput may not increase. Load balancing distributes flows; it normally does not split one TCP flow. Bonding requires a remote endpoint. Kernel MPTCP behavior varies and must be probed and tested.
