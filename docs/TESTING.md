# MultiWiFi Testing & Quality Assurance

This document outlines the automated testing strategy covering both the Rust workspace (`multiwifi-core`, `multiwifi-daemon`) and the C11/GTK4 GUI & packaging suites (`ctest`).

---

## 1. Rust Workspace Test Suites (`cargo test --workspace`)

All core networking primitives and the `zbus` system daemon are verified via Rust unit and integration tests compiled strictly with `cargo fmt --all -- --check` and `cargo clippy --workspace --all-targets -- -D warnings`:

- **`multiwifi-core` (38 tests):**
  - `config` (7 tests): TOML validation, strict schema version check, secret rejection, duplicate uplink check, symlink refusal, valid roundtrip.
  - `journal` (2 tests): journal save/load/remove roundtrip and symlink refusal.
  - `load_balancer` (4 tests): equal/unequal GCD WRR distribution, health penalty failover, multi-thread concurrency stress.
  - `bonding_protocol` (5 tests): `AuthReq`/`AuthResp` roundtrip, truncated/mismatched validation, 1-byte streaming, concatenated framing, garbage & bad-version resync around `0x4d574631`.
  - `capabilities` (2 tests): sync & async kernel/NM D-Bus capability probing within time budgets.
  - `firewall` (3 tests): invalid fwmark rejection, nftables dual-table rule builder (`ct mark` affinity + `flush`), forked isolated user+net namespace apply/verify/teardown lifecycle.
  - `netlink_route` (2 tests): CIDR stripping, isolated namespace `rtnetlink` rule & default route CRUD + verification (`table_id > 255`).
  - `loop_prevention` (2 tests): `127.0.0.0/8` skip behavior, isolated namespace `/32` host-route pin and rollback.
  - `routing_manager` (2 tests): conflict validation, isolated namespace apply -> verify -> teardown, partial failure atomic rollback, and orphaned `.journal` crash recovery.
  - `path_monitor` (4 tests): state string roundtrip, hysteresis (`3` successes / `3` failures), device disconnect/removal/manual updates, non-blocking `TcpSocket` port probing & `ECONNREFUSED` detection.
  - `tun_device` (1 test): non-blocking mock TUN `socketpair` open, IP validation, packet read/write, close lifecycle.
  - `bonding_client` (4 tests): secret token debug redaction, IPv4 anti-spoofing validator, auth rejection/invalid config rollback, end-to-end mock TCP server auth + TUN echo + spoofed packet drop + graceful disconnect.
- **`multiwifi-daemon` (10 tests):**
  - `operation` (1 test): bounded 50-entry UUID queue lifecycle and pruning.
  - `dns` (2 tests): validation (`1–16` IPv4/IPv6), atomic apply/backup/restore roundtrip, symlink traversal refusal.
  - `nm_client` (2 tests): NetworkManager state mapping and `/proc/net/route` gateway parsing.
  - `service` (5 tests): live Session D-Bus roundtrip (`GetVersion`, `GetCapabilities`, `GetDevices`, `GetPaths`, `GetStatus`, `SetPathWeight`, `StopLoadBalancing`, `StopBonding`, `RunDiagnostics` rate-limit, `GetDiagnostics`, `GetDNS`/`SetDNS`/`ResetDNS`), `OperationProgress`/`OperationFinished` signals, Polkit rejection on Session Bus, DNS/Bonding hardening checks, and runtime `multiwifi.conf` weight/disabled-uplink loading & secret rejection.

---

## 2. CMake / CTest GUI, Packaging & Live Parity Suites (`ctest`)

MultiWiFi enforces 5 CTest suites (for the retained C11/GTK4 GUI support layer, packaging/residual verification, and live C-to-Rust D-Bus parity) built cleanly with `-Wall -Wextra -Wpedantic`:

| Test Target | Focus & Verification Area |
| :--- | :--- |
| **`test_capabilities`** | Safe kernel probing and logging secret-redaction checks (`src/core/capabilities.c`, `logging.c`). |
| **`test_wifi_device`** | `libnm` device inventory structs and state transitions (`src/core/wifi_device.c`, `wifi_inventory.c`). |
| **`test_path_monitor`** | Hysteresis state machine, RTT smoothing, and mock socket probes (`src/core/path_monitor.c`). |
| **`test_packaging`** | D-Bus policy XML, Polkit rules, systemd unit sandboxing, desktop isolation, Arch/Debian/RPM manifests, portable GUI AppDir isolation, and isolated netns `libnftables` residual table check. |
| **`test_rust_parity`** | Live C `GDBusConnection` $\leftrightarrow$ compiled Rust `multiwifi-daemon` parity on an isolated `dbus-daemon`: XML introspection contract matching `data/io.github.multiwifi.Daemon1.xml`, status/capabilities/paths schemas, signals, rate limits, DNS `O_NOFOLLOW`, `Polkit` enforcement, `SIGTERM`/`StopDaemon` clean exit, and invalid-secret config resilience (`MULTIWIFI_BUILD_DAEMON=ON`). |

---

## 3. Running Automated Tests & Sanitizers

### Rust Lint & Test Suite
```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace -- --nocapture
```

### CMake Build & CTest Suite (GCC or Clang)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure --verbose

# Optional Clang C11 toolchain check (C GUI + core regression suites)
CC=clang cmake -B build-clang -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DMULTIWIFI_BUILD_DAEMON=OFF
cmake --build build-clang -j"$(nproc)"
ctest --test-dir build-clang --output-on-failure --verbose
```

### Memory Safety & Sanitizer Verification (ASan / LSan / UBSan on C Modules)
```bash
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMULTIWIFI_BUILD_DAEMON=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure --verbose
```

---

## 4. Network Namespace Isolation

Kernel routing, loop prevention, and `nftables` tests run inside forked child processes entering unprivileged Linux user + network namespaces (`unshare(CLONE_NEWUSER | CLONE_NEWNET)`), configuring `lo` directly via Netlink without touching host workstation routes or active Wi-Fi connections. In Rust tests, child panics are caught with `std::panic::catch_unwind` and mapped to non-zero exit codes checked by `waitpid`.
