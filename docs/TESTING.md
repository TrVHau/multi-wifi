# MultiWiFi Testing & Quality Assurance

This document outlines the testing strategy, test suites, network namespace isolation, sanitizer verification, and fault-injection scenarios for MultiWiFi.

---

## 1. Automated Test Suites (CTest)

MultiWiFi provides 9 comprehensive automated test suites implemented in C11 using the GLib Test Framework (`g_test_*`):

| Test Target | Component Under Test | Focus & Verification Area |
| :--- | :--- | :--- |
| **`test_config`** | `src/core/config.c` | TOML parsing, schema version validation, numeric range bounds, quotation sanitization, identifier safety. |
| **`test_capabilities`**| `src/core/capabilities.c` | Linux kernel version probing, MPTCP capability detection, nftables library checks, TUN device availability, NetworkManager D-Bus presence. |
| **`test_wifi_device`** | `src/core/wifi_device.c` | Device lifecycle, state machine transitions, hardware bus categorization (`PCIe`, `USB`), NetworkManager device mapping. |
| **`test_path_monitor`**| `src/core/path_monitor.c` | Multi-port socket fallback probe (DNS 53 -> HTTP 80 -> HTTPS 443), hysteresis (3 successes to recover, 3 failures to drop), RTT smoothing, loss calculation, D-Bus signal path updates, thread safety. |
| **`test_dbus_daemon`** | `src/daemon/service.c` | D-Bus IPC methods, lifecycle state machine, DNS methods (`GetDNS`, `SetDNS`, `ResetDNS`), Polkit authorization simulation, bounded operation queue (50 items max). |
| **`test_routing_manager`**| `src/core/routing_manager.c`| Network namespace isolation, policy routing tables ($201 \dots 200+N$), fwmark & source rule preferences ($11000 / 10000$), disk-backed journals, atomic rollback, recovery on startup. |
| **`test_load_balancer`**| `src/core/load_balancer.c` | Weighted Round-Robin scheduling, GCD weight normalization, flow hash distribution, nftables dual-table lifecycle (`inet` + `ip`), zero NAT table leaks. |
| **`test_bonding`** | `src/core/bonding_client.c` | MPTCP framed streaming protocol, magic header (`0x4D574246`) verification, auto-resync, HMAC token authentication, anti-spoofing packet validation (dropping loopback source IP), routing loop prevention. |
| **`test_packaging`** | `packaging/` | FHS filesystem compliance, systemd service security directives, AppImage desktop integration, Polkit rule syntax. |

---

## 2. Running Automated Tests

### Standard Test Execution
```bash
# Build with testing enabled
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j$(nproc)

# Run all 9 test suites
ctest --test-dir build --output-on-failure
```

### Verbose Execution of a Specific Test
```bash
# Run test_path_monitor with detailed step logs
./build/tests/test_path_monitor --verbose
```

---

## 3. Memory Safety & Sanitizer Verification (ASan / LSan / UBSan)

In accordance with `AGENTS.md`, MultiWiFi is regularly verified using LLVM/GCC sanitizers to guarantee **zero memory leaks** and **zero undefined behavior**:

```bash
# Configure and build with AddressSanitizer and UndefinedBehaviorSanitizer
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j$(nproc)

# Run test suite under sanitizers
ctest --test-dir build-asan --output-on-failure
```

### Sanitizer Quality Standards
- **Zero Heap Leaks:** All allocated structures (`MultiWifiPathInfo`, `MultiWifiDeviceInfo`, `GVariant`, `GPtrArray`) must have matching free functions.
- **Zero Use-After-Free (UAF):** Asynchronous GTK4 callbacks must track active async task counters (`active_async_tasks`) and cancel tokens safely.
- **Clean File Descriptor Lifetimes:** All descriptors must have `O_CLOEXEC` and be closed upon object disposal.

---

## 4. Network Namespace Isolation

Integration tests for policy routing and nftables execute inside temporary Linux network namespaces:
1. Creates an isolated network namespace (`ip netns add mw-test-ns`).
2. Creates virtual ethernet pairs (`veth0` $\leftrightarrow$ `veth1`).
3. Executes routing and netfilter modifications entirely within the isolated namespace.
4. **Safety Guarantee:** The host workstation's default routes, active Wi-Fi connections, and external network traffic are never touched or modified during testing.

---

## 5. Fault Injection Scenarios

MultiWiFi tests include negative and fault-injection cases:
- **Router Dropping Port 53:** Verified that `test_path_monitor` successfully falls back to port 80 and port 443 without false-negative link drops.
- **Malformed MPTCP Stream:** Verified that corrupted packets or garbage data injected into the bonding stream are discarded and the parser realigns to the next `0x4D574246` boundary.
- **Spoofed Packet Injection:** Verified that packets arriving via TUN with forged source address `127.0.0.1` are dropped immediately.
- **Unbounded Queue Overflow:** Verified that rapid creation of operations does not consume RAM beyond the 50-item limit.
- **Dialog Destruction During Async Operation:** Verified that closing dialog windows while scanning Wi-Fi or applying DNS does not cause heap corruption or crash.
