# MultiWiFi Dataplane & Network Architecture

This document details the Linux kernel dataplane, routing policies, netfilter rules, DNS subsystems, and MPTCP encapsulation mechanics implemented in `crates/multiwifi-core` and orchestrated by the Rust `multiwifi-daemon`.

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
| - Conntrack session affinity  |         | - Frame streaming (0x4d574631)|
| - Policy routing tables 10001+|         | - TUN device (mwtun0, MTU1420)|
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
To prevent broken HTTPS sessions, bank logins, and TLS handshakes, MultiWiFi uses connection tracking marks (`ct mark` masked with `0xff00`). Each new TCP/UDP connection is assigned to a specific Wi-Fi uplink fwmark (`0x1001`, `0x1002`, ...) on its first packet and remains pinned to that uplink for the duration of the flow.

MultiWiFi deploys **two dedicated nftables tables** with complete lifecycle isolation via thin `libnftables` FFI (`crates/multiwifi-core/src/firewall.rs`):

#### Table 1: Filter & Marking (`table inet multiwifi`)
Installed in the `inet` family to handle both IPv4 and IPv6 traffic:
- **`output` chain (`type route hook output priority mangle; policy accept;`):**
  - Restores existing connection mark: `ct mark & 0xff00 != 0 meta mark set ct mark accept`.
  - If unmarked, assigns a new mark across eligible uplinks via `numgen inc mod <N> map { ... }`, commits `ct mark set meta mark`, and accepts.
- **`prerouting` chain (`type filter hook prerouting priority mangle; policy accept;`):**
  - Restores marks (`ct mark & 0xff00 != 0 meta mark set ct mark`).

#### Table 2: NAT Masquerade (`table ip multiwifi`)
Installed in the `ip` family (best-effort) to rewrite source addresses to match the selected uplink:
- **`postrouting` chain (`type nat hook postrouting priority srcnat; policy accept;`):**
  ```nftables
  oifname != "lo" meta mark & 0xff00 != 0 masquerade
  ```
  Ensures that when a packet is routed out through a secondary interface such as `wlp0s20f0u1`, its source IP matches that interface's IP, preventing upstream ISP drops.

---

## 2.2. Policy Routing Tables & Rules
MultiWiFi provisions independent policy routing tables in the kernel using `rtnetlink` (`crates/multiwifi-core/src/netlink_route.rs` & `routing_manager.rs`):

| Parameter | Uplink 1 (`wlp0s20f3`) | Uplink 2 (`wlp0s20f0u1`) | Formula ($i = 1 \dots N$) |
| :--- | :--- | :--- | :--- |
| **Routing Table ID** | `10001` | `10002` | $10000 + i$ |
| **Source IP Rule** | `from 192.168.1.9 lookup 10001` | `from 192.168.1.213 lookup 10002` | Pref $20000 + i$ |
| **Fwmark Rule** | `fwmark 0x1001/0xff00 lookup 10001` | `fwmark 0x1002/0xff00 lookup 10002` | Pref $21000 + i$ |
| **Route Metric** | `100` | `110` | $100 + (i-1) \times 10$ |
| **Default Gateway** | `default via 192.168.1.1 dev wlp0s20f3` | `default via 192.168.1.1 dev wlp0s20f0u1` | Isolated per table |

- Packets bound explicitly to an IP or interface (`bind` / `SO_BINDTODEVICE`) hit the source IP rules (preference `20001+`).
- Normal application traffic marked by nftables hits the fwmark rules (preference `21001+`).
- If all paths degrade or go offline, the daemon tears down the firewall distribution marks to prevent blackholing traffic.

---

### 2.3. Reverse Path Filtering (`rp_filter`) Management
Linux defaults to Strict Reverse Path Filtering (`rp_filter = 1`), which drops returning packets if they arrive on an interface other than the best route.
1. When load balancing is activated, MultiWiFi queries and saves `orig_rp_filter` for each interface and for `/proc/sys/net/ipv4/conf/all/rp_filter`.
2. Switches `rp_filter` to Loose Mode (`2`), allowing asymmetric return traffic.
3. Upon teardown or rollback, MultiWiFi inspects `orig_rp_filter` and restores `1` only if that interface originally had strict filtering enabled.

---

## 3. MPTCP WAN Bonding Architecture

### 3.1. TUN Device (`mwtun0`) Encapsulation
- MultiWiFi creates a virtual Layer 3 point-to-point interface named `mwtun0` (`DEFAULT_TUN_NAME`) with a default MTU of **1420 bytes** (`IFF_TUN | IFF_NO_PI`).
- Opened with `O_RDWR | O_NONBLOCK | O_CLOEXEC` on `/dev/net/tun` (`crates/multiwifi-core/src/tun_device.rs`).
- Anti-spoofing validation (`is_valid_inbound_ip_packet`): Inbound packets with forged source IP addresses (loopback `127.0.0.0/8`, multicast `224.0.0.0/4`, broadcast `255.255.255.255`, or unspecified `0.0.0.0`) or invalid IPv4 header lengths are dropped immediately.

### 3.2. Framing Protocol (`0x4d574631`)
IP packets written to `mwtun0` are wrapped in a 16-byte binary header and sent over a non-blocking `IPPROTO_MPTCP` (`262`) socket with automatic fallback to TCP (`crates/multiwifi-core/src/bonding_protocol.rs` & `bonding_client.rs`):

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Magic Header (0x4d574631: 'M' 'W' 'F' '1')           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Version (1)  |  Frame Type   |             Flags             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Length                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload Data...                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```
- **Frame Types:** `0x01=AuthReq`, `0x02=AuthResp`, `0x03=IpPacket`, `0x04=Ping` (keepalive every 3s), `0x05=Pong`, `0x06=Disconnect`.
- **Byte Synchronization:** `FrameParser` caps buffering at 4 MiB and automatically resyncs to the next `0x4d574631` boundary (`BONDING_MAGIC`) if garbage bytes appear in the stream.

### 3.3. Routing Loop Prevention
To prevent tunnel traffic from recursively routing into `mwtun0`:
1. Before connecting to non-loopback endpoints, `LoopPrevention` (`crates/multiwifi-core/src/loop_prevention.rs`) adds a `/32` host route in table `main` pointing directly to the aggregation server via the physical Wi-Fi gateway and metric `10`.
2. Outgoing tunnel packets hit this `/32` host route and egress directly over the physical uplink.
3. Stopping the tunnel or hitting any failure during handshake automatically rolls back the `/32` host route via Netlink.

---

## 4. DNS Configuration Subsystem

Implemented in `crates/multiwifi-daemon/src/dns.rs`:
- **Presets Available in GUI:** Cloudflare (`1.1.1.1`, `1.0.0.1`), Google (`8.8.8.8`, `8.8.4.4`), Quad9 (`9.9.9.9`, `149.112.112.112`), AdGuard (`94.140.14.14`, `94.140.15.15`), and Custom IPv4/IPv6 resolvers (1–16 addresses).
- **Hardened File Management:**
  - Validates every entry via `std::net::IpAddr::parse`.
  - Refuses symlink traversal (`symlink_metadata` + `O_NOFOLLOW`) on `/etc/NetworkManager/conf.d/99-multiwifi-dns.conf` and `/run/multiwifi/resolv.conf.bak`.
  - Writes NetworkManager global DNS config atomically using temporary files and `fs::rename`.
  - Preserves `/etc/resolv.conf` if it is a symlink (such as `systemd-resolved`); otherwise backs it up to `/run/multiwifi/resolv.conf.bak` before atomic update and restores it on `ResetDNS` or daemon shutdown.
  - Reloads NetworkManager (`org.freedesktop.NetworkManager.Reload(0x3)`) asynchronously.

---

## 5. Transactional Journaling & Crash Recovery

Every routing mutation is recorded before execution in INI-formatted journals under `/run/multiwifi/journals/<OP_ID>.journal` (`crates/multiwifi-core/src/journal.rs`):
1. **Validation & Conflict Check:** Verifies non-reserved table IDs (`> 255`), empty target tables, and free RPDB priorities before touching kernel state.
2. **Readback Verification:** Verifies every added IP rule and route exists in kernel via `rtnetlink`.
3. **Atomic Rollback & Startup Recovery:** On partial failure or unclean shutdown, `RoutingManager::recover()` replays journal entries in reverse, removes orphaned netlink rules/routes, deletes stale `multiwifi` nftables tables, and unlinks recovered `.journal` files.
