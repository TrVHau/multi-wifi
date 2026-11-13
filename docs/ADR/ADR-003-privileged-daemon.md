# ADR-003: Privileged daemon

- Status: Accepted.
- Decision: Keep GUI unprivileged; route privileged operations through a D-Bus system daemon authorized by polkit.
- Consequences: Versioned API, packaging, authorization and strict input validation are required.
