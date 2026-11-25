# ADR-013: Phased Migration to Rust (Strangler Fig Strategy)

- **Status:** Accepted.
- **Supersedes (partial):** [ADR-001](ADR-001-c11-gtk4.md) for `multiwifi-core` and `multiwifi-daemon`.
- **Context:** While the C11 codebase passes strict compiler warnings, static analysis (`-fanalyzer`), and ASan/UBSan checks, maintaining manual memory ownership, GLib/GObject reference counting, raw `nlmsghdr`/`rtattr` netlink serialization, and multi-threaded non-blocking packet framing in C carries ongoing safety risk—especially inside the privileged `multiwifi-daemon` (`root`, `CAP_NET_ADMIN`, `CAP_NET_RAW`).
- **Decision:** Execute a phased Strangler Fig migration to Rust (`rustc >= 1.95`, Cargo workspace at repository root):
  1. **Migrate to Rust:** `multiwifi-core` (configuration, WRR load-balancer scheduler, binary bonding protocol, route journal, netlink routing manager, loop prevention, TUN/MPTCP bonding client, path monitor, capability probing) and `multiwifi-daemon` (`tokio` + `zbus` D-Bus service, async Polkit authorization, NetworkManager D-Bus client, hardened `O_NOFOLLOW` DNS management).
  2. **Retain C library FFI (`libnftables`):** Keep using the host `libnftables` C API through a minimal isolated Rust FFI wrapper rather than rewriting complex dual-table (`inet` + `ip`) ruleset generation via low-level netlink crates or shelling out to `nft`.
  3. **Retain C11 + GTK4 GUI (`multiwifi-gui`):** Keep the unprivileged GTK4 frontend in C11 communicating over the stable, versioned `io.github.multiwifi.Daemon1` D-Bus contract until the privileged backend migration is complete and battle-tested.
- **Consequences:**
  - `CMakeLists.txt` splits unprivileged GUI support (`multiwifi-gui-support`) from daemon-only C modules (`multiwifi-daemon-core`).
  - Both `ctest` (for C modules/GUI/packaging/D-Bus compatibility) and `cargo test` / `cargo clippy` must remain green at every phase.
