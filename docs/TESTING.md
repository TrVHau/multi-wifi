# Testing strategy

## Layers
1. Unit tests: parsers, schedulers, state machines and framing.
2. Component tests: libnm wrappers, D-Bus serialization and route-plan generation.
3. Integration: daemon and kernel networking in isolated namespaces.
4. Hardware: real adapters and independent access paths.
5. Release: clean install, upgrade, uninstall and recovery.

## Unit coverage
Test config parsing/migration, weighted scheduler, unhealthy-path exclusion, hysteresis, D-Bus validation, partial tunnel reads, oversized frames, journal serialization and secret redaction. Use deterministic clocks and fake monitors.

## Namespace integration
Use network namespaces and veth pairs; do not alter the host default route. Verify flow affinity, route table selection, conntrack marks, NAT/source selection, unhealthy-path exclusion, rollback ownership and daemon restart reconciliation.

## Bonding
Against a controlled remote endpoint, test authentication, invalid certificates, two independent MPTCP subflows, traffic over both paths, path loss, delay/loss/asymmetry, TUN loop prevention, MTU and leak prevention. A mock server does not prove Internet aggregation.

## Hardware matrix
Record distro/kernel, Wi-Fi chipset/driver/firmware, adapter count, simultaneous association support, NetworkManager version, MPTCP/path-manager capability, nftables version and WAN topology.

## Performance
Compare each uplink alone, best single link, multi-flow load balancing and remote bonding. Measure throughput, RTT, jitter, loss, CPU, memory and failover. Repeat tests and report median/spread. Ensure test server capacity is sufficient.

## Failure injection
Remove adapter, disable Wi-Fi, renew DHCP, kill daemon during apply, kill tunnel, restart NetworkManager, make server unreachable, fill queues, corrupt config and disconnect D-Bus.

All destructive tests must run in namespaces, VMs or dedicated test hardware.
