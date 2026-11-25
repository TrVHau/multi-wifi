# ADR-001: C11 and GTK4

- Status: Accepted for unprivileged GUI (`multiwifi-gui`); Superseded by [ADR-013](ADR-013-rust-migration.md) for `multiwifi-core` and `multiwifi-daemon`.
- Decision: Use C11, GTK4 and GLib/GIO for the native Linux application GUI (`multiwifi-gui`), while migrating privileged daemon and core networking modules incrementally to Rust per ADR-013.
- Rationale: Native Linux integration and direct access to GLib ecosystem APIs for GTK4 UI, paired with memory safety for privileged networking and wire parsing.
- Consequences: Carefully manage C/GLib ownership in `multiwifi-gui`; enforce Rust safety and strict `unsafe` isolation in `crates/`.
