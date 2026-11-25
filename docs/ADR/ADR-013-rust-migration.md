# ADR-013: Phased Migration to Rust (Strangler Fig Strategy)

- **Status:** Accepted & Implemented (Phases 0–5 Completed).
- **Supersedes (partial):** [ADR-001](ADR-001-c11-gtk4.md) for `multiwifi-core` and `multiwifi-daemon`.
- **Context:** While the C11 codebase passes strict compiler warnings, static analysis (`-fanalyzer`), and ASan/UBSan checks, maintaining manual memory ownership, GLib/GObject reference counting, raw `nlmsghdr`/`rtattr` netlink serialization, and multi-threaded non-blocking packet framing in C carries ongoing safety risk—especially inside the privileged `multiwifi-daemon` (`root`, `CAP_NET_ADMIN`, `CAP_NET_RAW`).
- **Decision:** Execute a phased Strangler Fig migration to Rust (`rustc >= 1.95`, Cargo workspace at repository root):
  1. **Migrate to Rust:** `multiwifi-core` (configuration, WRR load-balancer scheduler, binary bonding protocol, route journal, netlink routing manager, loop prevention, TUN/MPTCP bonding client, path monitor, capability probing) and `multiwifi-daemon` (`tokio` + `zbus` D-Bus service, async Polkit authorization, NetworkManager D-Bus client, hardened `O_NOFOLLOW` DNS management).
  2. **Retain C library FFI (`libnftables`):** Keep using the host `libnftables` C API through a minimal isolated Rust FFI wrapper rather than rewriting complex dual-table (`inet` + `ip`) ruleset generation via low-level netlink crates or shelling out to `nft`.
  3. **Retain C11 + GTK4 GUI (`multiwifi-gui`):** Keep the unprivileged GTK4 frontend in C11 communicating over the stable, versioned `io.github.multiwifi.Daemon1` D-Bus contract.
  4. **Retire legacy C daemon (`src/daemon/`):** Remove the legacy C daemon executable, C Polkit wrapper (`polkit-gobject-1`), and fallback build option (`MULTIWIFI_RUST_DAEMON`) once `test_rust_parity` verifies live C `GDBusConnection` $\leftrightarrow$ Rust daemon parity.
- **Consequences:**
  - `CMakeLists.txt` compiles `multiwifi-gui-support` + `multiwifi-gui` in C11/GTK4 and `multiwifi-daemon` unconditionally from `crates/multiwifi-daemon` (with `MULTIWIFI_BUILD_DAEMON=OFF` reserved strictly for C-only sanitizer/compiler test runs).
  - Remaining C core source modules under `src/core/` compile exclusively when `BUILD_TESTING=ON` (`multiwifi-core`) to sustain ASan/UBSan and cross-language schema regression coverage (`ctest`).
  - Both `ctest` and `cargo fmt` / `cargo clippy` / `cargo test` must remain green on every change.
