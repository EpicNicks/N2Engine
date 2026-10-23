# Task editor-tls: Native TLS for the editor server (#108 phase 2, engine half)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-editor-tls.

## Issue / plan
Approved by the user 2026-10-10 as proposed on #108 (engine half; the frontend half is a later item that needs this merged first). Scope: transport encryption and certificate pinning only. (1) Vendor mbedTLS (the 3.6 LTS line, unmodified, in external/mbedtls/, licence added to THIRD_PARTY_NOTICES.md, version recorded) behind a CMake option N2ENGINE_EDITOR_TLS, OFF by default, following how N2ENGINE_MODEL_UFBX and N2ENGINE_TEXT_FREETYPE are wired (compile definition, mirrored in the tests CMake files). Without the option the host behaves exactly as today and `--tls` is rejected with a clear message. (2) Host flag `--tls`: the editor port serves TLS 1.2+ (prefer 1.3 if the vendored build supports it). On first use the host generates a self-signed certificate (EC P-256) and keeps it, with its key, in a per-user location OUTSIDE the project tree so it can never be committed (find how user:// / the per-project user data resolves and put an `editor-tls/` directory beside it; if that is unclear, write needsUser). It prints the certificate SHA-256 fingerprint in the ready line: `N2EditorHost ready port=<port> tls=1 fingerprint=<64 lowercase hex, no colons>`; the frontend already ignores unknown key=value fields. (3) The transport seam is EditorServer Send/Receive/WaitUntilReady (editor-server/src/EditorServer.cpp): put one TLS session per accepted connection behind them. The handshake runs right after accept and must finish inside the existing Hello deadline (5 s); a failed or stalled handshake closes that connection and the server keeps accepting. WaitUntilReady selects on the fd, so it must also report bytes mbedTLS has already buffered (mbedtls_ssl_get_bytes_avail), or frames can stall. The Hello/token gate, the 64 KiB pre-Hello cap and the one-client-at-a-time rules are unchanged and apply over TLS. (4) The play child is launched by Electron on loopback and stays plaintext; document that `--tls` applies to one host process and that remote play mode stays disabled. Host.cpp already warns that a non-loopback bind sends the token unencrypted: keep that warning for plaintext and say so accurately when TLS is on. (5) No protocol.json change and no version bump. If the ready line or the transport is described in protocol.json and must change there, do not change it: write needsUser. (6) Tests (ASCII-only, compile on all 4 CI jobs; TLS tests compiled only with the option): HostOptions parsing of --tls and the ready line; a loopback handshake test with a client in the test using mbedTLS; Hello and a normal command over TLS; the buffered-bytes case (a frame larger than one TLS record); a garbage or stalled handshake does not wedge the accept loop; a plaintext client against a TLS host is refused. (7) CI: add N2ENGINE_EDITOR_TLS=ON to one existing Windows matrix leg (the PhysX OFF leg that already stacks optional features) and run the TLS tests there. (8) Docs: docs/logging-and-editor.html (#ready-line, #hello, and the Security and remaining gaps section #editor-open): the flag, the certificate location, the fingerprint and trust-on-first-use model, what TLS does not give (no per-user tokens, no rate limiting, no read-only mode, WAN throughput unmeasured). Add a "Next: Linux" note that the editor host currently builds and runs on Windows only, that the user wants a Linux host build next (cloud hosts are Linux), and that the TLS layer is written to be platform-neutral so that work does not redo it; list what the socket code still needs (Winsock names, int sockets, the build). Add a design-decision callout with Gains and Costs for the mbedTLS choice (vs OpenSSL and Schannel) and for TOFU pinning.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^external/mbedtls/
- ^tests/
- ^docs/
- CMakeLists.txt$
- ^THIRD_PARTY_NOTICES.md$
- ^.github/workflows/tests.yml$

## Acceptance
- TLS tests as listed, ASCII-only, compile on all 4 CI jobs
- All 4 Tests-workflow jobs green (the TLS leg runs the TLS tests)
- docs state the deferred hardening and the Linux-next note
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive: the reviewer should look hard at the handshake deadline, the buffered-bytes stall, certificate/key file handling and permissions, and that nothing logs the token or key. Not verifiable here: a real cloud host. List that under unverified. The frontend half (parse fingerprint, Node tls pinning, TOFU store, direct-address remote option) is a separate item after this merges.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs editor-tls` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-editor-tls` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/editor-tls/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-editor-tls --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/editor-tls/dev.json: {"pr":<number>,"branch":"feat-editor-tls","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-editor-tls` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
