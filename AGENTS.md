# Instructions for coding agents

## Mission
Implement MultiWiFi as a native Linux desktop application following the Strangler Fig Rust migration plan ([ADR-013](docs/ADR/ADR-013-rust-migration.md)): unprivileged GUI in C11 + GTK4, with `multiwifi-core` and `multiwifi-daemon` migrating incrementally to Rust. Read this file and relevant `docs/` before coding.

## Mandatory rules
1. Implement only the requested phase; do not jump ahead.
2. Use C11/GTK4/GLib for `multiwifi-gui` (and legacy C core not yet migrated); use idiomatic Rust (`edition = "2021"`, `cargo clippy --all-targets -- -D warnings`) for `crates/multiwifi-core` and future `crates/multiwifi-daemon`.
3. GUI runs unprivileged. Privileged networking changes go through a system daemon over D-Bus and polkit.
4. Never interpolate user-controlled input into shell commands. Prefer library APIs, D-Bus (`zbus`), `rtnetlink`, and `libnftables` FFI.
5. Never log Wi-Fi passwords, tokens, private keys or other secrets.
6. Validate all D-Bus input, configuration, network events and server responses.
7. Document GLib ownership (borrowed vs owned) in C code and document `# Safety` invariants on every `unsafe` block/fn in Rust (`#![deny(unsafe_op_in_unsafe_fn)]`).
8. Do not block the GTK main loop or async Tokio executor.
9. Every network change needs conflict detection, ownership tracking, verification and rollback.
10. Never flush global firewall rules or replace unrelated routes.
11. Do not claim MPTCP/bonding/failover works based only on compilation.
12. Add happy-path and failure-path tests (`cargo test` and `ctest`).
13. Report unsupported kernel capabilities clearly.
14. Do not claim a feature is complete unless build and tests pass.

## Workflow
Inspect repository; state a short plan; implement a small vertical slice; build with strict warnings (`clippy` / `-Wall -Wextra -Wpedantic`); run `cargo test` and `ctest`; review cleanup, authorization and rollback; update docs; report exact commands/results.

## Language standards
- **C11:** Enable `-Wall -Wextra -Wpedantic`; check fallible API calls; use `GError` consistently; avoid global mutable state; use monotonic time for deadlines; ensure thread-safe shared state.
- **Rust:** `#![deny(unsafe_op_in_unsafe_fn)]`; prefer `thiserror`/structured errors over `unwrap()` in production paths; minimize and isolate `unsafe` strictly to FFI boundaries; use monotonic `std::time::Instant` for deadlines.

## Completion report
List files changed, decisions, build command/result, test commands/results, known limitations and assumptions. Never say “done” if build/tests were not run.
