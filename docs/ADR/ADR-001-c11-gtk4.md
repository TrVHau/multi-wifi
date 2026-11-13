# ADR-001: C11 and GTK4

- Status: Accepted.
- Decision: Use C11, GTK4 and GLib/GIO for the native Linux application.
- Rationale: Native Linux integration and direct access to GLib ecosystem APIs.
- Consequences: Carefully manage C ownership; never block GTK main loop; test lifecycle and error paths.
