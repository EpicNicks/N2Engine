Phase 2 of #108, engine half: native TLS for the editor server. Transport encryption and certificate pinning only. The frontend half (parse the fingerprint, Node `tls` pinning, TOFU store, direct-address remote option) is a separate item that needs this merged first.

## What it does

- `--tls` makes the editor port serve TLS 1.2 or newer (1.3 when the client speaks it). Mbed TLS 3.6.7 (LTS) is vendored unmodified in `external/mbedtls/` and built into `N2EditorServer` only with `-DN2ENGINE_EDITOR_TLS=ON` (off by default, wired like `N2ENGINE_MODEL_UFBX`, mirrored in the tests CMake). Without the option the host is unchanged and `--tls` exits 1 with a clear message before the engine starts.
- First use makes a self-signed EC P-256 certificate and key in `<ProjectFile::UserDataBase()>/editor-tls/` (`%APPDATA%\N2Engine` on Windows, `$HOME/.n2engine` elsewhere), outside any project. The host refuses a folder that resolves under the project or when there is no user data folder. A single existing file, an invalid file or a mismatched pair is an error, never a silent replacement.
- The ready line gains `tls=1 fingerprint=<64 lowercase hex>` (SHA-256 of the DER certificate); a plaintext host's line is unchanged.
- The seam is `EditorServer::Send/Receive/WaitUntilReady`: one session per accepted connection (`EditorTls.hpp/.cpp`, no TLS header in a public header). The handshake runs after accept and shares the Hello deadline (counted from accept, with or without a token). A failed or stalled handshake closes the connection, never counts as a client or session, and the accept loop continues. `WaitUntilReady` also reports `mbedtls_ssl_get_bytes_avail` so a frame in the same record as the previous one does not stall. Hello/token gate, 64 KiB pre-Hello cap and one-client-at-a-time are unchanged.
- The play child stays plaintext (documented); remote play stays disabled. The non-loopback `--bind` warning stays for plaintext and says what is true when TLS is on.
- No `protocol.json` change, no version bump.

## Tests

`EditorTlsTests.cpp` (compiled only with the option; ASCII, LF): identity files, handshake and fingerprint, Hello and a command over TLS, TLS 1.2-only client, the token gate and pre-Hello cap and Hello deadline over TLS, several frames in one record, a frame larger than one record, garbage and stalled handshakes not wedging the accept loop, a plaintext client refused, a failed handshake not being a session for `--exit-on-disconnect`, nothing logging the token or key. `HostOptionsTests.cpp`: `--tls` parsing, usage, the ready line, and the no-TLS-build refusal.

## CI

The PhysX OFF leg (FreeType ON, ufbx ON) also builds `N2ENGINE_EDITOR_TLS=ON`, runs the TLS tests via ctest, and runs a `--tls` host smoke test (real host, `SslStream` client pinning the ready-line fingerprint, Hello, clean exit).

## Docs

`docs/logging-and-editor.html` (`#ready-line`, `#hello`, `#editor-open`, new `#editor-tls`): the flag, certificate location, fingerprint and trust-on-first-use, what TLS does not give (no per-user tokens, no rate limiting, no read-only mode, WAN throughput unmeasured), a "Next: Linux" note with what the socket code still needs, and design-decision callouts with Gains and Costs for Mbed TLS (vs OpenSSL and Schannel) and for TOFU pinning. `docs/testing.html` and `THIRD_PARTY_NOTICES.md` updated.

## Not verified

- Nothing was built or run locally; CI is the first build of all of this, including the vendored library on MSVC and the TLS 1.3 server path.
- A real cloud host, and any non-loopback network, WAN throughput or latency.
- The POSIX branches (socket shims in `EditorTls.cpp`, owner-only permissions on the key folder) have never been compiled or run.
- Windows ACLs on the key file were not inspected; it relies on the user profile folder's inherited permissions.


## Approved deviations (user, 2026-10-10)
- Certificate folder is <UserDataBase()>/editor-tls (one identity per user, shared by all projects), not per project, so the pinned fingerprint survives project changes; refused when UserDataBase falls back to '.' or resolves under the project.
- Cert and key are never regenerated or replaced silently: only one file, invalid files, or a mismatched pair is a startup error (delete both to reset).
- Handshake deadline is the Hello timeout counted from accept even with no access token (otherwise a stalled handshake would wedge the one-client accept loop); a Hello after it gets what is left.
- A failed handshake is closed before 'Editor connected', never sets _clientConnected or a session, so --exit-on-disconnect ignores it.
- Mbed TLS sources are compiled directly into N2EditorServer (no separate target) to avoid an export-set requirement; default mbedtls_config.h, nothing trimmed.
- Certificate validity is fixed 2024-2074 (client pins the fingerprint, host clock must not matter).
- Added a pwsh '--tls host smoke test' CI step (TLS leg only) beyond the brief, to exercise the real host path; --tls with --create/--play is accepted, not rejected.
- Mbed TLS threading is enabled (MBEDTLS_THREADING_C/ALT) through a separate user-config and threading_alt.h in editor-server/mbedtls-config/ with std::mutex callbacks; the vendored library is unmodified. Production code has no extra locking.
