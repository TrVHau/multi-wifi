# Implementation roadmap

0. **Foundation:** repository, build, GTK shell, logging, config schema, tests and CI. No network mutation.
1. **Wi-Fi inventory:** read-only libnm integration; list adapters, connection state, signal, addresses and gateways.
2. **Path monitor:** independently probe each uplink; add hysteresis and diagnostics.
3. **System daemon:** D-Bus, polkit and operation lifecycle; start with read-only methods.
4. **Routing transaction manager:** inspect state, detect conflicts, journal, apply/verify/rollback in namespaces.
5. **Load balancing:** conntrack flow affinity and dedicated policy-routing tables; test NAT, DHCP, IPv4/IPv6.
6. **Bonding:** remote protocol, authentication, MPTCP, TUN framing, loop prevention and server forwarding.
7. **Release:** Arch, Debian and RPM packages, GUI-only AppImage, clean install/upgrade/uninstall and security review.

A phase is complete only when build and tests pass, failure behavior is covered, docs match implementation, unsupported capabilities are not presented as active, and rollback is verified where state is changed.
