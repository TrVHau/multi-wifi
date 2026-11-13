# ADR-010: Secret management

- Status: Accepted.
- Decision: Delegate Wi-Fi secrets to NetworkManager and desktop Secret Service; store bonding credentials in a secret store.
- Consequences: No plaintext credentials in ordinary config or logs. Support rotation and redaction.
