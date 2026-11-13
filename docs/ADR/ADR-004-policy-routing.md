# ADR-004: Policy routing

- Status: Accepted.
- Decision: Use dedicated routing tables and rules per managed uplink.
- Consequences: Detect conflicts, preserve unrelated routes and implement transactional apply/verify/rollback.
