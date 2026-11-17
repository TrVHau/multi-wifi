# ADR-012: Dual inet and ip nftables Architecture for Filtering and NAT Masquerading

- **Status:** Accepted.
- **Context:** Linux netfilter supports the unified `inet` family for packet filtering (output and prerouting chains) across IPv4 and IPv6, but certain kernel versions require the `ip` family for NAT postrouting masquerading. Early versions of the application attempted conditional deletion during teardown, leading to unreleased `table ip multiwifi_table` NAT rules (`CWE-772`).
- **Decision:** Architect the netfilter engine with two dedicated tables: `table inet multiwifi_table` for flow connection marking (`ct mark`) and packet routing marks, and `table ip multiwifi_table` exclusively for dynamic postrouting masquerading (`oif != "lo" meta mark != 0 masquerade`). Enforce unconditional deletion and verification of both tables during teardown.
- **Consequences:** Ensures clean packet marking across all protocols while guaranteeing 100% leak-free teardown of masquerade NAT rules upon disabling load balancing.
