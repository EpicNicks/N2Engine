# Task net-a5-handshake-aead: Netcode A5: connection handshake with a stateless challenge cookie, per-connection keys and AEAD-sealed packets on Mbed TLS primitives, join tokens, and the shared security policy library moved out of editor-server
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-a5-handshake-aead.

## Issue / plan
Netcode design C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read sections 1.3 (threat model), 2.1 (2.1.1, 2.1.3 FULLY), 2.2, the A5 row of 7.2 and the risks; the editor security designs C:/Method Notes Obsidian/orchestrator/proposals/security-design-lua-editmode-and-preauth-slots.md and C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md (PreAuthSlots, TokenTable hashing, rate limits, fail-closed bind) as the precedent. MERGED prerequisites: A1 presets/settings (#187), A2 simulation clock, A3 n2net core (channels, ITransport, LoopbackTransport), A4 native UdpTransport (#203: fail-closed non-loopback bind unless --insecure-bind; its API takes an explicit 'encryption available' parameter that is false today: A5 makes it true when the secure layer is on). Mbed TLS 3.6.7 is vendored in external/mbedtls but only built for the editor host behind N2ENGINE_EDITOR_TLS. SCOPE (the design recommends our own AEAD packet layer with an ECDHE handshake using Mbed TLS primitives, reviewed against the netcode.io standard: follow it; record any departure as a deviation): (1) BUILD: an Mbed TLS crypto target (crypto only: no X.509/TLS needed) built for the native game/server builds on every native leg; the web build skips it (the browser supplies transport security later, B10) and must still compile; (2) SHARED POLICY LIB: move TokenBucket, RateLimiter, PreAuthSlots and TokenTable from editor-server into an engine-side library both use, with NO behaviour change for the editor host (its tests must pass unchanged apart from include paths); (3) HANDSHAKE (a layer between ITransport and the A3 channels): client sends a PADDED connect request (request size >= reply size: no amplification) carrying a protocol id/version and a join-token proof; the server answers with a STATELESS challenge cookie (HMAC over source address + coarse time + a per-process secret, short expiry) and allocates NOTHING; the client echoes the cookie; only then does the server consume a PreAuthSlots slot and complete the key exchange (ECDHE, e.g. X25519 if Mbed TLS 3.6 provides it in the vendored config, else P-256: verify in the headers); session keys via HKDF per direction; (4) AEAD packets (ChaCha20-Poly1305 or AES-GCM: pick what the vendored config has, verify) with nonce = per-direction packet sequence, a replay window, packets failing auth dropped silently and counted; (5) JOIN TOKENS: v1 static tokens file with roles (player, spectator, admin) hashed like the editor's TokenTable (SHA-256), constant-time compare, never logged (redaction like the editor log lines); (6) the A4 fail-closed rule becomes: non-loopback bind allowed when the secure layer is on; --insecure-bind still required otherwise; (7) TESTS: handshake success, wrong token refused, cookie forged/expired/from another address refused, no slot allocated before the cookie echo (slot count assertion), the AMPLIFICATION BOUND (server reply bytes <= request bytes for every pre-auth message), replay of a sealed packet dropped, tampered packet dropped, nonce reuse impossible by construction, rate limit on connect floods, loopback end to end with A3 channels over UdpTransport plus the secure layer; bounded run time; (8) DOCS: docs/netcode.html (handshake diagram in text, threat model coverage, what is NOT covered: no forward secrecy beyond ECDHE per session if that holds, no certificate identity of the server: say exactly what a client can and cannot trust), docs/testing.html. THIS ITEM NEEDS THE USER'S SECURITY-REVIEW ACCEPT AT MERGE: put a 'Security review' section in the PR body listing every cryptographic choice, the exact primitives and parameters, what is and is not authenticated, and the known limits. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^external/mbedtls/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Stateless cookie handshake; no server state before the echo; amplification bound tested
- ECDHE + HKDF per-direction keys; AEAD with sequence nonces and a replay window; tamper/replay tests
- Join tokens hashed, constant-time compare, never logged
- Policy classes moved with no editor-host behaviour change
- Web still compiles; all checks green; docs verified
- PR body has a Security review section for the user
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security): nonce uniqueness per key and direction, replay window correctness, key separation per direction, cookie binding (address, time, secret rotation), constant-time compares, no allocation/state before cookie, amplification, token redaction in logs, error paths that leak timing or oracle info, Mbed TLS API misuse (return codes checked, contexts freed), the editor host unchanged after the move, docs claims exact. Mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a5-handshake-aead` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-a5-handshake-aead` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a5-handshake-aead/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-a5-handshake-aead --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a5-handshake-aead/dev.json: {"pr":<number>,"branch":"feat-net-a5-handshake-aead","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-a5-handshake-aead` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
