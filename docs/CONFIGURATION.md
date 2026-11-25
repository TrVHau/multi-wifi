# MultiWiFi Configuration Guide

This document describes the configuration file format, TOML schema specifications, runtime directory layouts, and validation rules implemented by `crates/multiwifi-core/src/config.rs` (and mirrored in `src/core/config.c`).

---

## 1. Configuration File Locations

| Path | Purpose | Ownership / Permissions |
| :--- | :--- | :--- |
| `/etc/multiwifi/multiwifi.conf` | Primary system configuration file (TOML, no secrets; loaded at daemon startup or via `MULTIWIFI_CONFIG_PATH`) | `root:root` (`0644`) |
| `/run/multiwifi/journals/` | Ephemeral routing journals for atomic rollback | `root:root` (`0700`) |
| `/etc/NetworkManager/conf.d/99-multiwifi-dns.conf` | Managed NetworkManager global DNS drop-in | `root:root` (`0644`) |
| `/run/multiwifi/resolv.conf.bak` | Automatic backup of static `/etc/resolv.conf` | `root:root` (`0644`) |

> [!IMPORTANT]
> In accordance with [ADR-010](ADR/ADR-010-secrets.md), **secrets are strictly forbidden** inside `multiwifi.conf`. Any file containing keys or keywords such as `password`, `secret`, `private_key`, `token`, `psk`, or `passphrase` is rejected on load with `SecretForbidden`. Runtime bonding tokens and server endpoints are transmitted only on demand via D-Bus (`StartBonding`).

---

## 2. Configuration Schema & Format

MultiWiFi parses strict TOML (`#[serde(default, deny_unknown_fields)]`) and rejects unknown keys as well as symbolic links (`O_NOFOLLOW`).

### Complete Valid Configuration Example

```toml
# MultiWiFi Configuration File
schema_version = 1
mode = "load_balance"              # Options: "load_balance", "bonding", "none"

[load_balance]
strategy = "weighted_round_robin"  # Supported strategy
health_check_interval_ms = 2000    # Interval in ms (100 - 60000)
failure_threshold = 3              # Consecutive failures to drop link (1 - 50)
recovery_threshold = 3             # Consecutive successes to recover link (1 - 50)
cooldown_ms = 5000                 # Flapping cooldown period in ms (0 - 600000)

[[uplinks]]
device_id = "wlp0s20f3"
enabled = true
weight = 100                       # Scheduler weight (1 - 1000)

[[uplinks]]
device_id = "wlp0s20f0u1"
enabled = true
weight = 80                        # Lower weight for slower adapter

[bonding]
enabled = false
profile_id = "home-vpn-cluster"    # Non-secret profile identifier
```

---

## 3. Section & Key Reference

### Root Table
- **`schema_version`** *(integer, default: `1`)*:
  - Must be `1`. Any other value is rejected with `ConfigError::SchemaVersion`.
- **`mode`** *(string, default: `"load_balance"`)*:
  - Operational mode: `"load_balance"`, `"bonding"`, or `"none"`.

---

### `[load_balance]` Table
- **`strategy`** *(string, default: `"weighted_round_robin"`)*:
  - Traffic scheduling strategy (`"weighted_round_robin"`).
- **`health_check_interval_ms`** *(integer, default: `2000`)*:
  - Time between background socket health checks (`100..=60000` ms).
- **`failure_threshold`** *(integer, default: `3`)*:
  - Consecutive probe failures required to transition from `Online`/`Degraded` to `Local Only` (`1..=50`).
- **`recovery_threshold`** *(integer, default: `3`)*:
  - Consecutive probe successes required to promote back to `Online` (`1..=50`).
- **`cooldown_ms`** *(integer, default: `5000`)*:
  - Delay before re-promoting a recently dropped path (`0..=600000` ms).

---

### `[[uplinks]]` Array of Tables
Each entry configures a managed Wi-Fi interface:
- **`device_id`** *(string, required, unique)*:
  - Interface identifier containing only ASCII alphanumeric characters, `-`, or `_`. Duplicate IDs are rejected.
- **`enabled`** *(boolean, default: `true`)*:
  - Whether this uplink participates in balancing.
- **`weight`** *(integer, default: `1`)*:
  - Relative scheduler weight (`1..=1000`).

---

### `[bonding]` Table
- **`enabled`** *(boolean, default: `false`)*:
  - Whether WAN tunnel bonding mode is selected.
- **`profile_id`** *(string, default: `""`)*:
  - Identifier for the bonding profile (`alphanumeric`, `-`, `_` when set). Server host, port, and secret auth tokens are provided at runtime through D-Bus `StartBonding`.

---

## 4. DNS Settings & Presets

System DNS is managed dynamically via the GUI DNS dialog or D-Bus (`GetDNS` / `SetDNS` / `ResetDNS`):

| Provider | Primary DNS | Secondary DNS | Characteristics |
| :--- | :--- | :--- | :--- |
| **Cloudflare** | `1.1.1.1` | `1.0.0.1` | Low latency, DNS filtering bypass |
| **Google Public DNS** | `8.8.8.8` | `8.8.4.4` | Global anycast, high uptime |
| **Quad9** | `9.9.9.9` | `149.112.112.112` | Malware & phishing protection |
| **AdGuard DNS** | `94.140.14.14` | `94.140.15.15` | Ad and tracker blocking |
| **Custom** | *User-defined* | *User-defined* | Accepts `1–16` valid IPv4 or IPv6 addresses |

---

## 5. Validation & Security Guarantees

- Files larger than 1 MiB or located via symbolic links (`O_NOFOLLOW`) are rejected.
- Out-of-range integers, negative weights, unknown TOML keys, and duplicate uplink `device_id`s fail validation with structured `ConfigError` values without mutating daemon runtime state.
