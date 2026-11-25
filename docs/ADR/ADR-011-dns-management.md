# ADR-011: Safe Atomic DNS Switching with Anti-Symlink Protection

- **Status:** Accepted.
- **Context:** Users with dual Wi-Fi uplinks frequently experience DNS resolution conflicts, slow domain lookups from secondary ISP resolvers, or site blocks imposed by local ISP DNS. The application required a native mechanism to query, override, and restore system resolvers.
- **Decision:** Implement `GetDNS`, `SetDNS`, and `ResetDNS` in `multiwifi-daemon` writing a NetworkManager global DNS drop-in (`/etc/NetworkManager/conf.d/99-multiwifi-dns.conf`) and issuing a non-blocking NetworkManager `Reload` D-Bus call. Reject symlinks on managed config/backup targets via `g_file_test(..., G_FILE_TEST_IS_SYMLINK)`. When `/etc/resolv.conf` is a static non-symlink file, back it up to `/run/multiwifi/resolv.conf.bak` before atomic replacement with `g_file_set_contents()`.
- **Consequences:** Eliminates DNS conflicts across dual uplinks; preserves `systemd-resolved` stub symlinks while mitigating symlink-traversal writes (`CWE-59`); restores original static resolver settings cleanly on reset or daemon stop.
