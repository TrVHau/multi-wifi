# ADR-005: Flow affinity

- Status: Accepted.
- Decision: Assign new flows to uplinks and preserve selection with conntrack marks.
- Consequences: Ordinary TCP flows are not packet-striped. Existing flows may fail when their path disappears.
