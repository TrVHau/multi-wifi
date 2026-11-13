# Network design

## Two distinct modes

### Load balancing
Assign each new flow to one uplink and pin it for the flow lifetime. This can distribute many flows across links. It does not normally make one TCP connection use the sum of two unrelated gateways.

### Bonding
Use a remote aggregation endpoint and a multipath transport such as MPTCP. The server receives traffic over multiple subflows and forwards it onward. This requires server capacity and compatible routing.

## Components
- `WifiManager`: NetworkManager/libnm device and profile integration.
- `InterfaceInventory`: ifindex, addresses, MTU, carrier and routes.
- `PathMonitor`: gateway/internet reachability, RTT, loss and throughput estimates.
- `CapabilityDetector`: kernel, MPTCP, nftables and TUN support.
- `RoutingManager`: plan, apply, verify and rollback.
- `FirewallManager`: application-owned nftables rules and conntrack marks.
- `LoadBalancer`: select uplink for each new flow.
- `BondingController`: tunnel lifecycle and endpoint authentication.
- `StateStore`: desired config separate from observed runtime state.

## Discovery and health
Use libnm as the source of truth for Wi-Fi devices and connection profiles. For each path collect interface, ifindex, active connection UUID, addresses, gateway, DNS, route metric, signal quality and independent usability. Association does not prove Internet reachability.

Suggested states: `DISCONNECTED -> ASSOCIATING -> CONNECTED_LOCAL -> PROBING -> ONLINE | DEGRADED | OFFLINE`. Use consecutive probe thresholds and hysteresis.

## Load balancing dataplane
Create a dedicated routing table per uplink. Add rules matching only MultiWiFi-owned packet marks. Use nftables/conntrack to select a path on the first packet of a new flow, store the selection in the conntrack mark, and restore it for subsequent packets.

Start with weighted round-robin and health filtering. Adaptive scheduling should follow measurement. Do not packet-stripe an ordinary TCP flow across unrelated gateways: asymmetric routing and NAT state can break connections.

Each path may have a different source address and upstream NAT. Ensure source-address selection and masquerading match the marked path. Test independent gateways, shared LAN, DHCP renewal, gateway changes, IPv4 and IPv6.

Do not promise migration of established TCP flows after path failure. Stop assigning new flows to unhealthy paths; existing flows may fail.

## DNS and IPv6
Make DNS behavior explicit and detect resolver conflicts. Either implement IPv6 policy routing/leak prevention or explicitly state that the first release does not manage IPv6. Never claim full-system coverage if IPv6 bypasses policy.

## Bonding dataplane
A remote endpoint is mandatory. Probe MPTCP support at runtime: socket support, path management, required sysctls/permissions, remote handshake and established subflow count. Kernel behavior varies.

For full-system routing, create a TUN interface and encapsulate packets over an authenticated transport. Pin outer transport sockets to physical uplinks and exclude the server endpoint from the TUN default route to prevent loops. On tunnel failure, restore safe routing.

If IP packets are framed over an ordered MPTCP byte stream, define version, type, payload length and payload. Handle partial reads and multiple frames, validate lengths, and bound queues. Ordered streams can cause head-of-line blocking; this is not equivalent to a multipath UDP tunnel.

Use TLS 1.3 or another reviewed authenticated transport; do not invent cryptography. Specify server forwarding, NAT, MTU and abuse controls separately.

## Transactional network changes
1. Inspect current state and conflicts.
2. Build a desired-state plan.
3. Validate capabilities and authorization.
4. Apply only owned objects.
5. Read back and verify kernel state.
6. Commit runtime state.
7. Roll back only objects created by this operation on failure.

Persist a recovery journal. Never flush global firewall tables or unrelated routes.

## Failover and measurement
Use failure/recovery streaks and cooldown. Report reasons and timestamps. Measure single-flow throughput, many-flow throughput, RTT, jitter, loss, CPU, memory, failover and recovery. Compare against each individual link and the best single link; do not promise the sum of link rates.
