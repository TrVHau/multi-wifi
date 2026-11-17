# ADR-011: Safe Atomic DNS Switching with Anti-Symlink Protection

- **Status:** Accepted.
- **Context:** Users with dual Wi-Fi uplinks frequently experience DNS resolution conflicts, slow domain lookups from secondary ISP resolvers, or site blocks imposed by local ISP DNS. The application required a native mechanism to query, override, and restore system resolvers.
- **Decision:** Implement `GetDNS`, `SetDNS`, and `ResetDNS` in `multiwifi-daemon` using an atomic file replacement strategy with explicit symlink traversal defense (`O_NOFOLLOW` and `lstat()` inspection). Provide presets for Cloudflare, Google, Quad9, AdGuard, and custom resolvers. Automatically back up the original resolver configuration to `/etc/resolv.conf.multiwifi-backup` before any mutation.
- **Consequences:** Eliminates DNS conflicts across dual uplinks; completely mitigates symlink traversal attacks (`CWE-59`); ensures atomic and rollback-safe resolver transitions without leaving corrupted `/etc/resolv.conf` files.
