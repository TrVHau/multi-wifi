# Configuration and runtime state

Keep desired configuration separate from observed kernel/NetworkManager state. Cached status is not proof that a route or tunnel still exists.

Suggested paths:
- `/etc/multiwifi/`: system configuration
- `$XDG_CONFIG_HOME/multiwifi/`: user preferences
- `/run/multiwifi/`: runtime files
- `/var/lib/multiwifi/`: persistent daemon journal
- system journal: logs

Example (illustrative TOML):
```toml
schema_version = 1
mode = "load_balance"

[load_balance]
strategy = "weighted_round_robin"
health_check_interval_ms = 2000
failure_threshold = 3
recovery_threshold = 3
cooldown_ms = 5000

[[uplinks]]
device_id = "wifi-a"
enabled = true
weight = 1

[[uplinks]]
device_id = "wifi-b"
enabled = true
weight = 1

[bonding]
enabled = false
profile_id = ""
```

Validate schema, enums, numeric ranges, device IDs and server profiles. Never accept arbitrary commands or firewall snippets. Do not store secrets in ordinary config.

Version persistent schemas and test deterministic migrations. Back up before replacement; if migration fails, retain original and start safely disabled.

Journal operation ID, daemon generation, owned routes/rules, nftables identifiers, TUN identity and last verified state. After reboot inspect actual kernel state; do not blindly replay stale state.
