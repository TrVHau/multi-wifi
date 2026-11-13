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

## Phases

0 Foundation; 1 Wi-Fi inventory; 2 path monitor; 3 privileged daemon; 4 routing transaction manager; 5 load balancing; 6 MPTCP bonding; 7 packaging and release.

## Constraints

Two independent Wi-Fi paths generally require two adapters. If both links share one constrained WAN, throughput may not increase. Load balancing distributes flows; it normally does not split one TCP flow. Bonding requires a remote endpoint. Kernel MPTCP behavior varies and must be probed and tested.
