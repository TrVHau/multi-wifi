# MultiWiFi D-Bus Interface Specification

This document defines the complete D-Bus IPC specification for `multiwifi-daemon`. The service exposes an introspectable, versioned interface over the System D-Bus.

---

## 1. Service Coordinates

| Coordinate | Value | Description |
| :--- | :--- | :--- |
| **Bus Type** | System D-Bus | Served via `zbus` (`crates/multiwifi-daemon`), consumed by GTK4 GUI (`GDBusConnection`) |
| **Well-Known Name** | `io.github.multiwifi.Daemon` | Owned by `multiwifi-daemon` |
| **Object Path** | `/io/github/multiwifi/Daemon` | Primary service singleton |
| **Interface** | `io.github.multiwifi.Daemon1` | Primary daemon interface |

---

## 2. Authorization & Policy

- **Read-Only Methods:** Unprivileged access allowed for all local users.
- **Mutating Methods:** Enforced via Polkit action `io.github.multiwifi.modify`. If unauthorized, the method returns `io.github.multiwifi.Error.NotAuthorized`.

---

## 3. Methods

### `GetVersion() -> (version: s, api_version: u)`
Returns the MultiWiFi daemon semantic version string and integer API version.
- **Returns:**
  - `version` (`s`): e.g. `"0.1.0"`
  - `api_version` (`u`): `1`

---

### `GetCapabilities() -> (capabilities: a{sv})`
Queries current host kernel and platform capabilities.
- **Returns:** Dictionary of capabilities:
  - `kernel_version` (`s`): Host Linux kernel release string
  - `mptcp_state` (`s`): `"Active"`, `"Available (Inactive)"`, `"Unsupported"`, or `"Unknown"`
  - `mptcp_details` (`s`): Diagnostic details on kernel MPTCP support/sysctl
  - `nftables_state` (`s`): `"Active"`, `"Available (Inactive)"`, `"Unsupported"`, or `"Unknown"`
  - `tun_state` (`s`): `"Active"`, `"Available (Inactive)"`, `"Unsupported"`, or `"Unknown"`
  - `network_manager_state` (`s`): `"Active"`, `"Available (Inactive)"`, `"Unsupported"`, or `"Unknown"`

---

### `GetDevices() -> (devices: aa{sv})`
Returns an array of all detected Wi-Fi devices and their current connection status.
- **Returns:** Array of dictionaries:
  - `ifname` (`s`): Interface name (e.g. `"wlp0s20f3"`)
  - `ifindex` (`i`): Kernel interface index
  - `hw_address` (`s`): MAC address
  - `state` (`s`): Human-readable device state (`"Connected"`, `"Disconnected"`, `"Unmanaged"`, etc.)
  - `ssid` (`s`): Connected SSID name or `""`
  - `bssid` (`s`): Connected BSSID or `""`
  - `signal_strength` (`y`): Signal quality percentage (0–100)
  - `frequency` (`u`): Channel frequency in MHz (e.g. `2412`)
  - `ipv4_address` (`s`): Assigned IPv4 CIDR (e.g. `"192.168.1.9/24"`)
  - `ipv4_gateway` (`s`): Default gateway IP (e.g. `"192.168.1.1"`)
  - `ipv6_address` (`s`): Assigned IPv6 CIDR or `""`
  - `connection_uuid` (`s`): Active NetworkManager profile UUID or `""`
  - `connection_id` (`s`): Active NetworkManager profile ID or `""`
  - `managed` (`b`): Whether NetworkManager manages this interface

---

### `GetPaths() -> (paths: aa{sv})`
Returns latency and reachability metrics across all active network paths.
- **Returns:** Array of dictionaries:
  - `ifname` (`s`): Interface name
  - `gateway` (`s`): Gateway IP
  - `source_ip` (`s`): Local source IP
  - `state` (`s`): Path health (`"Online"`, `"Degraded"`, `"Local Only"`, `"Probing"`, `"Offline"`)
  - `status_reason` (`s`): Diagnostic status message
  - `rtt_ms` (`d`): Smoothed round-trip time in milliseconds
  - `min_rtt_ms` (`d`): Minimum observed RTT
  - `max_rtt_ms` (`d`): Maximum observed RTT
  - `jitter_ms` (`d`): Estimated jitter in milliseconds
  - `loss_percent` (`u`): Packet loss percentage ($0 \dots 100$)
  - `eligible` (`b`): Whether this path is currently eligible for traffic

---

### `GetStatus() -> (status: a{sv})`
Queries the aggregated runtime status of the multi-path engines.
- **Returns:**
  - `version` (`s`): Daemon semantic version string (e.g. `"0.1.0"`)
  - `mode` (`s`): Current mode (`"idle"`, `"load_balancing"`, `"bonding"`)
  - `daemon_running` (`b`): Always `TRUE` while reachable
  - `routing_active` (`b`): Whether policy routing load balancing is active
  - `firewall_active` (`b`): Whether nftables rules are installed
  - `bonding_active` (`b`): Whether MPTCP bonding is connected
  - `bonding_state` (`s`): Detailed tunnel state (`"Disconnected"`, `"Connecting"`, `"Authenticating"`, `"Connected"`, `"Disconnecting"`, `"Failed"`)
  - `bonding_mptcp` (`b`): Whether the tunnel socket negotiated `IPPROTO_MPTCP`
  - `bonding_virtual_ip` (`s`): Assigned TUN virtual IPv4 address or `""`
  - `eligible_uplinks` (`u`): Number of eligible interfaces in scheduler
  - `polkit_enforced` (`b`): Whether Polkit authorization checks are active

---

### `StartLoadBalancing(options: a{sv}) -> (operation_id: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Activates per-flow Weighted Round-Robin load balancing across all eligible active Wi-Fi adapters.
- **Parameters:**
  - `options` (`a{sv}`): Optional key-value settings.
- **Returns:**
  - `operation_id` (`s`): Unique UUID for tracking progress.

---

### `StopLoadBalancing() -> (operation_id: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Safely tears down policy routing, removes nftables marks, restores sysctl settings, and performs atomic verification.
- **Returns:**
  - `operation_id` (`s`): Tracking operation ID.

---

### `StartBonding(profile: a{sv}) -> (operation_id: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Starts MPTCP tunnel aggregation through a remote bonding relay server.
- **Parameters:**
  - `server_host` (`s`): Server IP or hostname (required, non-empty)
  - `server_port` (`u`): TCP port `1..65535` (optional, defaults to `5000`)
  - `token` (`s`): Authentication secret token (required, non-empty)
  - `profile_id` (`s`): Optional profile identifier (defaults to `"default"`)
- **Returns:**
  - `operation_id` (`s`): Tracking operation ID.

---

### `StopBonding() -> (operation_id: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Tears down the `mwtun0` tunnel device and restores original default routing.
- **Returns:**
  - `operation_id` (`s`): Tracking operation ID.

---

### `SetPathWeight(device_id: s, weight: u)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Manually adjusts the scheduler weight of a specific network interface.
- **Parameters:**
  - `device_id` (`s`): Interface name (e.g. `"wlp0s20f3"`)
  - `weight` (`u`): Weight value ($1 \dots 1000$)

---

### `RunDiagnostics() -> (operation_id: s)`
Triggers an immediate asynchronous probe round across all paths (rate-limited to once per 1.5s; returns `Busy` if called more frequently).
- **Returns:**
  - `operation_id` (`s`): Tracking operation ID.

---

### `GetDiagnostics(operation_id: s) -> (result: a{sv})`
Retrieves detailed diagnostic metrics for a completed diagnostic run.
- **Parameters:**
  - `operation_id` (`s`): Operation ID from `RunDiagnostics`.
- **Returns:**
  - `result` (`a{sv}`): Diagnostic report dictionary.

---

### `GetDNS() -> (servers: as, is_custom: b)`
Queries the current system DNS configuration and identifies whether a custom override is in place.
- **Returns:**
  - `servers` (`as`): Array of active DNS server IP strings (e.g. `["1.1.1.1", "1.0.0.1"]`)
  - `is_custom` (`b`): `TRUE` if MultiWiFi has overridden DNS; `FALSE` if original DHCP default.

---

### `SetDNS(servers: as) -> (result: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Configures system resolvers via `/etc/NetworkManager/conf.d/99-multiwifi-dns.conf` (with backup/update of static `/etc/resolv.conf` if non-symlink) and reloads NetworkManager DNS configuration asynchronously. Refuses symlinks on managed targets (`CWE-59`).
- **Parameters:**
  - `servers` (`as`): Array of 1–16 valid IPv4 or IPv6 DNS addresses.
- **Returns:**
  - `result` (`s`): `"OK"`

---

### `ResetDNS() -> (result: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Removes the MultiWiFi NetworkManager drop-in, restores static `/etc/resolv.conf` from `/run/multiwifi/resolv.conf.bak` if present, and reloads NetworkManager DNS configuration asynchronously.
- **Returns:**
  - `result` (`s`): `"OK"`

---

### `StopDaemon()`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Requests clean daemon termination and rolls back any active network modifications.

---

## 4. Signals

| Signal | Signature | Description |
| :--- | :--- | :--- |
| `DeviceChanged` | `(a{sv} device)` | Emitted when a Wi-Fi device connects, disconnects, or changes IP. |
| `PathChanged` | `(a{sv} path)` | Emitted when link latency, loss, or health state changes. |
| `StatusChanged` | `(a{sv} status)` | Emitted when operational mode transitions (e.g. active / idle). |
| `OperationProgress`| `(s operation_id, u percent, s message)` | Progress notification for long-running operations. |
| `OperationFinished`| `(s operation_id, b success, a{sv} result)` | Completion event for an asynchronous operation. |
| `Warning` | `(s code, s message)` | Emitted on non-fatal events (e.g. link degraded). |

---

## 5. Standard Errors

All errors are returned under the `io.github.multiwifi.Error.*` domain:

| Error Name | Cause |
| :--- | :--- |
| `NotAuthorized` | Polkit authorization failed or was denied by the user. |
| `InvalidArgument` | Malformed IP address, invalid interface name, or out-of-range weight. |
| `Unsupported` | Kernel lacks required capabilities (MPTCP, nftables, or TUN). |
| `Conflict` | Route collision or existing conflicting rule detected. |
| `CapabilityMissing` | Required subsystem (e.g. NetworkManager) is offline. |
| `NetworkFailure` | Netlink error, interface missing, or failed socket binding. |
| `RollbackFailed` | Teardown could not remove all objects (journal preserved). |
| `Busy` | Another mutating operation is currently in progress. |

---

## 6. CLI Usage Examples

```bash
# 1. Query general daemon status
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.GetStatus

# 2. Query real-time path latency
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.GetPaths

# 3. Start Load Balancing
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.StartLoadBalancing "{}"

# 4. Set Cloudflare DNS
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.SetDNS "['1.1.1.1', '1.0.0.1']"

# 5. Revert DNS back to DHCP
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.ResetDNS

# 6. Stop Load Balancing
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon1.StopLoadBalancing
```
