# Instructions for coding agents

## Mission
Implement MultiWiFi as a native Linux desktop application in C. Read this file and relevant `docs/` before coding.

## Mandatory rules
1. Implement only the requested phase; do not jump ahead.
2. Use C11, GTK4, GLib/GIO and libnm.
3. GUI runs unprivileged. Privileged networking changes go through a system daemon over D-Bus and polkit.
4. Never interpolate user-controlled input into shell commands. Prefer library APIs, D-Bus and rtnetlink.
5. Never log Wi-Fi passwords, tokens, private keys or other secrets.
6. Validate all D-Bus input, configuration, network events and server responses.
7. Document GLib ownership: borrowed versus owned references.
8. Do not block the GTK main loop.
9. Every network change needs conflict detection, ownership tracking, verification and rollback.
10. Never flush global firewall rules or replace unrelated routes.
11. Do not claim MPTCP/bonding/failover works based only on compilation.
12. Add happy-path and failure-path tests.
13. Report unsupported kernel capabilities clearly.
14. Do not claim a feature is complete unless build and tests pass.

## Workflow
Inspect repository; state a short plan; implement a small vertical slice; build with warnings; run tests; review cleanup, authorization and rollback; update docs; report exact commands/results.

## C standards
C11; enable `-Wall -Wextra -Wpedantic`; check fallible API calls; use `GError` consistently; avoid global mutable state; use monotonic time for deadlines; ensure thread-safe shared state.

## Completion report
List files changed, decisions, build command/result, test commands/results, known limitations and assumptions. Never say “done” if build/tests were not run.
