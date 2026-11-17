# MultiWiFi D-Bus Interface Specification

This document defines the complete D-Bus IPC specification for `multiwifi-daemon`. The service exposes an introspectable, versioned interface over the System D-Bus.

---

## 1. Service Coordinates

| Coordinate | Value | Description |
| :--- | :--- | :--- |
| **Bus Type** | `G_BUS_TYPE_SYSTEM` | System Message Bus |
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
  - `network_manager` (`s`): `"active"`, `"inactive"`, or `"unsupported"`
  - `kernel_mptcp` (`s`): `"active"`, `"inactive"`, or `"unsupported"`
  - `nftables` (`s`): `"active"`, `"inactive"`, or `"unsupported"`
  - `tun_device` (`s`): `"active"`, `"inactive"`, or `"unsupported"`

---

### `GetDevices() -> (devices: aa{sv})`
Returns an array of all detected Wi-Fi devices and their current connection status.
- **Returns:** Array of dictionaries:
  - `ifname` (`s`): Interface name (e.g. `"wlp0s20f3"`)
  - `state` (`s`): Device state (`"connected"`, `"disconnected"`, `"unmanaged"`)
  - `ssid` (`s`): Connected SSID name or `""`
  - `ipv4_address` (`s`): Assigned IPv4 CIDR (e.g. `"192.168.1.9/24"`)
  - `ipv4_gateway` (`s`): Default gateway IP (e.g. `"192.168.1.1"`)
  - `signal_strength` (`u`): Signal quality percentage ($0 \dots 100$)
  - `frequency` (`u`): Channel frequency in MHz (e.g. `2412`)
  - `hw_bus` (`s`): Hardware bus type (`"PCIe"`, `"USB"`, `"Internal"`)

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
  - `mode` (`s`): Current mode (`"idle"`, `"load_balancing"`, `"bonding"`)
  - `load_balancing_active` (`b`): Whether flow balancing is active
  - `bonding_active` (`b`): Whether MPTCP bonding is active
  - `firewall_active` (`b`): Whether nftables rules are installed
  - `eligible_paths` (`u`): Number of eligible interfaces

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
  - `server_host` (`s`): Server IP or hostname
  - `server_port` (`u`): TCP port (e.g. `5000`)
  - `auth_token` (`s`): Authentication secret token
- **Returns:**
  - `operation_id` (`s`): Tracking operation ID.

---

### `StopBonding() -> (operation_id: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Tears down the `mw-tun0` tunnel device and restores original default routing.
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
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Triggers an immediate asynchronous probe round across all paths and verifies kernel routing tables.
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
Atomically configures system resolvers in `/etc/resolv.conf` using hardened replacement.
- **Parameters:**
  - `servers` (`as`): Array of valid IPv4 or IPv6 DNS addresses.
- **Returns:**
  - `result` (`s`): `"success"`

---

### `ResetDNS() -> (result: s)`
*(Requires Polkit: `io.github.multiwifi.modify`)*  
Restores the original system DNS configuration from `/etc/resolv.conf.multiwifi-backup`.
- **Returns:**
  - `result` (`s`): `"success"`

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
    --method io.github.multiwifi.Daemon.GetStatus

# 2. Query real-time path latency
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.GetPaths

# 3. Start Load Balancing
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StartLoadBalancing "{}"

# 4. Set Cloudflare DNS
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.SetDNS "['1.1.1.1', '1.0.0.1']"

# 5. Revert DNS back to DHCP
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.ResetDNS

# 6. Stop Load Balancing
gdbus call --system \
    --dest io.github.multiwifi.Daemon \
    --object-path /io/github/multiwifi/Daemon \
    --method io.github.multiwifi.Daemon.StopLoadBalancing
```
