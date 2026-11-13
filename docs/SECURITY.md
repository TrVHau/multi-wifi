# Security and privilege model

## Assets and threats
Protect Wi-Fi credentials, bonding credentials, routing/firewall state and tunnel traffic. Threats include unauthorized local callers, malformed D-Bus input, compromised endpoints, secret leakage, daemon crashes and conflicts with VPN/container networking.

## Process separation
GUI runs as the logged-in user with no root privileges. A separate system daemon performs privileged operations through a narrow D-Bus API and polkit authorization. The daemon validates every argument independently.

## D-Bus/polkit
Separate read-only and mutating actions. Use D-Bus credentials and polkit subject identity; do not trust a caller-supplied username. Reject unknown enum values, oversized strings, malformed addresses and out-of-range numbers. Rate-limit expensive operations.

## Secrets
Delegate Wi-Fi secrets to NetworkManager and desktop Secret Service. Store bonding credentials in an appropriate secret store. Never place secrets in ordinary config, logs, diagnostics or crash reports. Support rotation and revocation.

## Tunnel
Use TLS 1.3 or another reviewed authenticated transport. Verify server identity and certificates. Bound frame sizes, queues and concurrent sessions. Reject protocol downgrade and malformed frames. Never implement custom cryptography.

## Network ownership
Use a dedicated nftables table and identifiable route/rule metadata. Detect conflicts with VPNs, firewalld, ufw and containers. Never flush global firewall rules. Journal changes and rollback on failure. Uninstall only resources demonstrably owned by MultiWiFi.

## Process safety
Avoid shell commands. Prefer libraries and netlink. If a subprocess is unavoidable, use fixed executable paths and argument vectors. Validate interface names, IDs, addresses, ports and paths. Protect privileged file operations against symlink attacks.

## Logging and tests
Use structured logs; redact secrets and avoid payload capture. Test unauthorized D-Bus calls, malformed requests, invalid certificates, oversized frames, daemon crash/restart and safe uninstall.
