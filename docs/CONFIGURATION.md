# MultiWiFi Configuration Guide

This document describes the configuration file format, schema specifications, runtime directory layouts, and validation rules for MultiWiFi.

---

## 1. Configuration File Locations

| Path | Purpose | Ownership / Permissions |
| :--- | :--- | :--- |
| `/etc/multiwifi/multiwifi.conf` | Primary system configuration file | `root:root` (0644 or 0600 if secrets present) |
| `/run/multiwifi/journals/` | Ephemeral routing journals for atomic rollback | `root:root` (0700) |
| `/etc/resolv.conf.multiwifi-backup` | Backup copy of original system resolver | `root:root` (0644) |

---

## 2. Configuration Schema & Format

MultiWiFi uses a standard TOML syntax parsed by `src/core/config.c`.

### Complete Configuration Example

```toml
# MultiWiFi Configuration File
schema_version = 1
mode = "load_balance"  # Options: "load_balance", "bonding", "none"

[load_balance]
strategy = "weighted_round_robin"  # Options: "weighted_round_robin", "round_robin"
health_check_interval_ms = 2000    # Health check interval in milliseconds (500 - 30000)
failure_threshold = 3              # Consecutive probe failures to drop link (1 - 10)
recovery_threshold = 3             # Consecutive probe successes to restore link (1 - 10)
cooldown_ms = 5000                 # Flapping cooldown period in ms (0 - 60000)

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
profile_id = "home-vpn-cluster"
server_host = "198.51.100.1"       # Aggregation relay server IP or hostname
server_port = 5000                 # Relay server MPTCP listening port
auth_token = "secret-token-here"   # HMAC authentication token
mtu = 1420                         # Virtual TUN MTU (1280 - 1500)
```

---

## 3. Section & Key Reference

### Root Table
- **`schema_version`** *(integer, required)*:
  - Must be `1`. Other versions will be rejected with `MULTIWIFI_CONFIG_ERROR_SCHEMA_VERSION`.
- **`mode`** *(string, required)*:
  - Operational mode: `"load_balance"`, `"bonding"`, or `"none"`.

---

### `[load_balance]` Table
- **`strategy`** *(string, optional, default: `"weighted_round_robin"`)*:
  - Traffic scheduling strategy.
- **`health_check_interval_ms`** *(integer, optional, default: `2000`)*:
  - Time between consecutive background link probes (in milliseconds). Minimum: `500`, Maximum: `30000`.
- **`failure_threshold`** *(integer, optional, default: `3`)*:
  - Number of consecutive probe failures required to mark an interface as `DEGRADED` or `LOCAL_ONLY`.
- **`recovery_threshold`** *(integer, optional, default: `3`)*:
  - Number of consecutive probe successes required to promote an interface back to `ONLINE`.
- **`cooldown_ms`** *(integer, optional, default: `5000`)*:
  - Anti-flapping delay before re-evaluating an unstable link.

---

### `[[uplinks]]` Array of Tables
Each entry represents an explicit configuration for a physical network interface:
- **`device_id`** *(string, required)*:
  - Network interface name (e.g. `"wlp0s20f3"`, `"wlp0s20f0u1"`). Must contain only alphanumeric characters, dashes, or underscores.
- **`enabled`** *(boolean, optional, default: `true`)*:
  - Whether MultiWiFi should manage and balance traffic over this adapter.
- **`weight`** *(integer, optional, default: `100`)*:
  - Relative traffic weight ($1 \dots 1000$). Higher weight assigns proportionally more flows.

---

### `[bonding]` Table
- **`enabled`** *(boolean, optional, default: `false`)*:
  - Whether WAN tunnel bonding is active.
- **`profile_id`** *(string, optional)*:
  - Identifier for the remote aggregation profile.
- **`server_host`** *(string, required if bonding enabled)*:
  - IP address or domain name of the remote MPTCP aggregation relay server.
- **`server_port`** *(integer, optional, default: `5000`)*:
  - Listening port of the aggregation server ($1 \dots 65535$).
- **`auth_token`** *(string, required if bonding enabled)*:
  - Shared secret used for token-based authentication with the server.
- **`mtu`** *(integer, optional, default: `1420`)*:
  - MTU for the `mw-tun0` TUN virtual interface. Must be between `1280` and `1500`.

---

## 4. DNS Settings & Presets

System DNS can be managed dynamically via GUI or D-Bus API. The default DNS presets provided by MultiWiFi are:

| Provider | Primary DNS | Secondary DNS | Characteristics |
| :--- | :--- | :--- | :--- |
| **Cloudflare** | `1.1.1.1` | `1.0.0.1` | Lowest latency, unblocks ISP-restricted sites |
| **Google Public DNS** | `8.8.8.8` | `8.8.4.4` | Global anycast, high uptime |
| **Quad9** | `9.9.9.9` | `149.112.112.112` | Automatic malware & phishing domain blocking |
| **AdGuard DNS** | `94.140.14.14` | `94.140.15.15` | Ad and tracking telemetry filtration |
| **Custom** | *User-defined* | *User-defined* | Supports any valid IPv4 or IPv6 resolver |

---

## 5. Validation & Error Handling

MultiWiFi performs strict validation upon parsing `multiwifi.conf`:
- Missing required keys or invalid schema versions trigger `MULTIWIFI_CONFIG_ERROR_VALIDATION`.
- Numeric values outside permissible ranges are rejected with descriptive errors.
- Quotation marks (both `'` and `"`) are automatically sanitized.
- If the configuration file is unparseable or corrupted, `multiwifi-daemon` rejects the mutation and retains its running state without risking network disruption.
