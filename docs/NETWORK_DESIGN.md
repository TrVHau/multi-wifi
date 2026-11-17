# MultiWiFi Dataplane & Network Architecture

This document details the Linux kernel dataplane, routing policies, netfilter rules, DNS subsystems, and MPTCP encapsulation mechanics implemented in MultiWiFi.

---

## 1. Operating Modes Overview

```text
+-------------------------------------------------------------------------+
|                  Concurrent Physical Wi-Fi Interfaces                   |
|           Adapter 1: wlp0s20f3           Adapter 2: wlp0s20f0u1         |
|         (PCIe, Viettel Telecom)          (USB, FPT Telecom)             |
+------------------------------------+------------------------------------+
                                     |
                +--------------------+--------------------+
                |                                         |
                v                                         v
+-------------------------------+         +-------------------------------+
|     Mode 1: Load Balancing    |         |        Mode 2: WAN Bonding    |
+-------------------------------+         +-------------------------------+
| - nftables flow mark          |         | - MPTCP transport socket      |
| - Conntrack session affinity  |         | - Frame streaming (0x4D574246)|
| - Policy routing tables 201+  |         | - TUN device (mw-tun0, MTU1420|
| - Dynamic WRR scheduling      |         | - Host-route loop prevention  |
| - No remote server required   |         | - Requires aggregation relay  |
+-------------------------------+         +-------------------------------+
                |                                         |
                v                                         v
     Multiple Internet Flows                   Single Aggregated Stream
  (Web, Downloads, P2P, Streaming)             (High-Throughput Single Pipe)
```

---

## 2. Multi-Path Load Balancing Architecture

### 2.1. Conntrack Flow Affinity & nftables Rules
To prevent broken HTTPS sessions, bank logins, and TLS handshakes, MultiWiFi uses connection tracking marks (`ct mark`). Each new TCP/UDP connection is assigned to a specific Wi-Fi uplink on its first packet and remains pinned to that uplink for the duration of the flow.

MultiWiFi deploys **two dedicated nftables tables** with complete lifecycle isolation:

#### Table 1: Filter & Marking (`table inet multiwifi_table`)
Installed in the `inet` family to handle both IPv4 and IPv6 traffic:
- **`output` chain (hook output, priority -150):**
  - Inspects locally generated packets.
  - Restores existing connection mark: `ct mark != 0 meta mark set ct mark`.
  - If unmarked, assigns a new mark calculated by the Weighted Round-Robin scheduler and commits it: `ct mark set meta mark`.
- **`prerouting` chain (hook prerouting, priority -150):**
  - Restores marks for forwarded or returning traffic.

#### Table 2: NAT Masquerade (`table ip multiwifi_table`)
Installed in the `ip` family to rewrite source addresses to match the selected uplink:
- **`postrouting` chain (hook postrouting, priority 100):**
  ```nftables
  oif != "lo" meta mark != 0 masquerade
  ```
  Ensures that when a packet is routed out through `wlp0s20f0u1`, its source IP is translated to that interface's IP (`192.168.1.213`), preventing upstream ISP drops.

---

### 2.2. Policy Routing Tables & Rules
MultiWiFi provisions independent policy routing tables in the kernel using `rtnetlink`:

| Parameter | Uplink 1 (`wlp0s20f3`) | Uplink 2 (`wlp0s20f0u1`) | Formula ($i = 1 \dots N$) |
| :--- | :--- | :--- | :--- |
| **Routing Table ID** | `201` | `202` | $200 + i$ |
| **Source IP Rule** | `from 192.168.1.9/24 lookup 201` | `from 192.168.1.213/24 lookup 202` | Pref $10000 + i$ |
| **Fwmark Rule** | `fwmark 0x20000001/0xffffffff lookup 201` | `fwmark 0x20000002/0xffffffff lookup 202` | Pref $11000 + i$ |
| **Route Metric** | `100` | `110` | $100 + (i-1) \times 10$ |
| **Default Gateway** | `default via 192.168.1.1 dev wlp0s20f3` | `default via 192.168.1.1 dev wlp0s20f0u1` | Isolated per table |

- Packets bound explicitly to an IP or interface (`SO_BINDTODEVICE`) hit the source IP rules (preference 10000).
- Normal application traffic marked by nftables hits the fwmark rules (preference 11000).
- If an interface fails or goes offline, its fwmark rule is dynamically removed from traffic assignment by the scheduler.

---

### 2.3. Reverse Path Filtering (`rp_filter`) Management
Linux defaults to Strict Reverse Path Filtering (`rp_filter = 1`), which drops returning packets if they arrive on an interface other than the best route.
1. When load balancing is activated, MultiWiFi queries and saves `orig_rp_filter` for each interface and for `/proc/sys/net/ipv4/conf/all/rp_filter`.
2. Switches `rp_filter` to Loose Mode (`2`), allowing asymmetric return traffic.
3. Upon teardown, MultiWiFi inspects `orig_rp_filter` and only restores `1` if that interface originally had `1`, preserving existing user/VPN configurations.

---

## 3. MPTCP WAN Bonding Architecture

### 3.1. TUN Device (`mw-tun0`) Encapsulation
- MultiWiFi creates a virtual Layer 3 point-to-point interface named `mw-tun0` with an MTU of **1420 bytes** (leaving 80 bytes for outer IP, MPTCP, and framing headers).
- Created securely using `open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC)`.
- Anti-spoofing validation: Packets arriving from the tunnel with forged source IP addresses (e.g. `127.0.0.1/8`) are rejected and dropped immediately.

### 3.2. Framing Protocol (`0x4D574246`)
IP packets written to the TUN device are read by `multiwifi-daemon`, wrapped in a framed streaming protocol, and multiplexed over an MPTCP socket:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Magic Header (0x4D574246: 'M' 'W' 'B' 'F')              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Version (1) |  Flags / Type |        Payload Length         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Sequence Number                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |            Reserved           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Data...                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```
- **Byte Synchronization:** If stream bytes are corrupted or lost, the parser scans for the `0x4D574246` magic boundary and realigns automatically.
- **Throughput:** Exceeds 1,465 MB/s (~11.72 Gbps) in software parsing microbenchmarks.

### 3.3. Routing Loop Prevention
To prevent tunnel traffic from recursively routing into itself:
1. MultiWiFi installs a `/32` host route pointing directly to the aggregation relay server via the physical Wi-Fi gateway:
   ```bash
   ip route add <SERVER_IP>/32 via <GATEWAY_IP> dev <PHYSICAL_IFNAME>
   ```
2. The default route (`0.0.0.0/0`) is redirected through `mw-tun0`.
3. Outgoing tunnel TCP packets match the explicit `/32` host route and escape via the physical uplinks.

---

## 4. DNS Configuration Subsystem

To accelerate domain lookups and bypass ISP restrictions across dual uplinks:
- **Presets Available in GUI:**
  - **Cloudflare DNS:** `1.1.1.1`, `1.0.0.1` (Fastest, unblocks sites)
  - **Google Public DNS:** `8.8.8.8`, `8.8.4.4` (High global reliability)
  - **Quad9:** `9.9.9.9`, `149.112.112.112` (Malware & phishing protection)
  - **AdGuard DNS:** `94.140.14.14`, `94.140.15.15` (Ad and tracker blocking)
  - **Custom Servers:** Supports arbitrary user-specified IPv4 and IPv6 resolvers.
- **Atomic File Management:**
  - Validates IP addresses using `inet_pton(AF_INET)` and `inet_pton(AF_INET6)`.
  - Automatically creates a backup at `/etc/resolv.conf.multiwifi-backup`.
  - Writes to a temporary file with `O_NOFOLLOW` (protecting against symlink traversal attacks) and renames atomically to `/etc/resolv.conf`.
  - Restores original DHCP configuration upon clicking **Reset to DHCP** or running `ResetDNS`.

---

## 5. Transactional Journaling & Crash Recovery

MultiWiFi records every network modification in a disk-backed journal at `/run/multiwifi/journals/<OP_ID>.journal`:
1. **Journal Structure:**
   - JSON-formatted state recording created routing tables, rule preferences, interface marks, and sysctl states.
2. **Crash Recovery on Boot:**
   - When `multiwifi-daemon` initializes, it automatically invokes `multiwifi_routing_manager_recover()`.
   - Reads any uncommitted journals from `/run/multiwifi/journals/`, removes orphaned netlink rules, deletes residual nftables tables, and cleans up temporary state.
