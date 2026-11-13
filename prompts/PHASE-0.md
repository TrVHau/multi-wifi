# Coding-agent prompt — Phase 0: Foundation

Read `AGENTS.md`, `README.md` and relevant `docs/` first.

## Objective
Create a clean, buildable C11/GTK4 project foundation. Do not implement Wi-Fi control, privileged routing, load balancing or bonding yet.

## Deliverables
1. One documented build system and build/test workflow.
2. Minimal GTK4 application window that runs as an ordinary user.
3. Clear source layout for GUI, core, daemon stub, shared D-Bus definitions and tests.
4. GLib structured logging setup.
5. Versioned config schema/parser scaffold and validation tests; no secrets.
6. Capability/status model that can represent unsupported features.
7. Unit-test target and smoke tests for config/state model.
8. Exact developer build/run/test documentation.
9. CI workflow for Linux formatting/build/tests.

## Constraints
C11, GTK4, GLib/GIO. No root requirement. No shell-based networking commands. No network state changes. Never fake Wi-Fi/bonding status. Keep GTK responsive. Enable compiler warnings and resolve project-owned warnings.

## Acceptance
A clean checkout builds using documented commands; GUI launches/exits; tests pass without Wi-Fi hardware or root; README commands are accurate.

Before coding, inspect the repository and present a short plan. Implement Phase 0 only. Finish with the completion report required by `AGENTS.md`.
