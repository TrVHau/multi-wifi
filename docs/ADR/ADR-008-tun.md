# ADR-008: TUN for full-system bonding

- Status: Accepted for design.
- Decision: Use TUN for full-system IP encapsulation over authenticated MPTCP transport.
- Consequences: Prevent routing loops; handle MTU, DNS, IPv4/IPv6, bounded queues and tunnel failure. Ordered streams can cause head-of-line blocking.
