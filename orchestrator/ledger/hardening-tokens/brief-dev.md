# Task hardening-tokens: Exposed host hardening PR B: per-user tokens (admin/editor/viewer), tokens file, --new-token, audit lines, protocol 1.12.0
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-hardening-tokens.

## Issue / plan
Design and the user's DECISIONS: C:/Method Notes Obsidian/orchestrator/proposals/exposed-host-hardening.md (read it fully first, especially sections 2a, 3 and 'Decisions (user, 2026-10-13)'). PR A (read-only classification, --read-only, IsReadCommand default-deny with the exhaustive protocol.json classification test, ErrorCoded response + Hello errorCodes flag, ServerInfo.readOnly, protocol 1.11.0) is MERGED (#126): build on it; re-read EditorServer.cpp, HostOptions.cpp, protocol.json and the protocol docs at current master first (line numbers in the proposal are stale). Scope for PR B: (1) `--tokens-file <path>`: JSON array of {user, role (admin|editor|viewer), hash 'sha256:<64 lowercase hex>'}; hashes verified with Mbed TLS SHA-256 (user decision: the tokens file REQUIRES a TLS build N2ENGINE_EDITOR_TLS; with TLS OFF --tokens-file is a clear startup error); compare the SHA-256 of the presented token against ALL entries with a constant-time loop, no early exit; reject malformed entries (bad role, short or non-hex hash, duplicate user) at startup with a clear error and never log token material. `--token-env` stays (single synthetic admin user 'token') and is mutually exclusive with --tokens-file (startup error). (2) `N2EditorHost --new-token <user> <role>`: prints a fresh 256-bit random token ONCE (OS CSPRNG, e.g. the mbedtls RNG setup the TLS code already uses) and the JSON line to add; machine-generated tokens make unsalted SHA-256 adequate. (3) Roles bound to the connection at Hello: admin = everything; editor = everything except Shutdown and the other Admin-class commands (use the Read/Write/Admin classes from PR A; editor may Write, only admin may Admin); viewer = strictly read-only AND may NOT enter play mode (SendInput, WritePlaySnapshot and the play commands are denied for viewers); a denial uses the PR A ErrorCoded 'forbidden' code (plain-Error fallback for old clients). The --read-only host switch still applies on top. (4) Revocation without restart: re-stat the file (mtime+size) on each accepted connection and, on POSIX, on SIGHUP (set a flag in the handler, reload on the network thread); a failed reload keeps the old table and logs a Warn; a reload that removes (or changes the role of) the user of the open connection closes it (flag checked in the HandleClient loop); all table swaps on the network thread. POSIX only: refuse to start when the tokens file is group/world readable (skipped on Windows). (5) Identity: log `user=<u> role=<r>` on every Hello success and failure (peer address comes with PR C, not here) and an `audit:` Info line per non-polled mutating command naming the user; ServerInfo gets trailing `user` and `role`, and Hello gets a trailing optional `user` string (absent from older clients: the token is then matched across all entries; present: only that user's entry is considered, still constant-time). Protocol 1.12.0 (minor, trailing fields only): update protocol.json (follow the version rule in its header and the optionalTrailing/lenient-decoder keys PR A added), regenerate editor-server/clients/{cpp,csharp,web} with the generators, test-vectors.json and the TS codec tests, shortPayloads vectors for the new trailing fields; old clients and old hosts must keep working. (6) Tests (socket/unit tests that run on every leg; the SHA-256 dependent tests only where N2ENGINE_EDITOR_TLS is ON, guarded like the existing TLS tests): SHA-256 known-answer vectors; token matching picks the right user/role; wrong token or user; roles enforced (viewer denied a write and play, editor denied Shutdown, admin allowed); --token-env is admin and exclusive with the file; revoke by rewriting the file between two connections (portable); open-connection revocation closes it; permissions refusal (POSIX only, skipped on Windows); malformed-file startup errors; log lines contain user/role but never the token or its hash. (7) Docs: docs/editor.html and docs/logging-and-editor.html (tokens file format, --new-token workflow, roles table, revocation, audit lines, what is NOT covered: no rate limiting until PR C, serial-server starvation risk). Do NOT do rate limiting (PR C) or deploy/Docker/systemd changes (PR D). Do NOT change the frontend repo (a separate PR follows). Nothing can be built locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`); confirm the Dockerfile and Linux legs build with TLS ON as the proposal asks, and report what you found.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/
- ^cmake/
- ^CMakeLists\.txt$
- ^THIRD_PARTY_NOTICES\.md$

## Acceptance
- All Tests-workflow jobs green
- Per-user tokens with roles enforced; viewer cannot write or play; editor cannot Shutdown
- Revocation by file rewrite works; tokens never logged
- protocol 1.12.0 clients regenerated and compatible with 1.10/1.11 peers
- Docs describe the file format, --new-token and roles
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive: reviewer does a full security review (constant-time compare with no early exit, no token or hash in logs or errors, file perms, TOCTOU on reload, role bound at Hello, viewer play denial, default-deny classification still exhaustive). Unverified: real exposed host, SIGHUP through systemd/docker, Windows ACLs.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs hardening-tokens` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-hardening-tokens` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/hardening-tokens/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-hardening-tokens --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/hardening-tokens/dev.json: {"pr":<number>,"branch":"feat-hardening-tokens","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-hardening-tokens` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
