# Task net-a6-n2server: Netcode A6: N2Server headless dedicated-server executable (ready line, tokens file, join-token CLI)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-a6-n2server.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md: row A6 of the stage table (line ~294), the N2Server paragraph (line ~141: --project, --scene, --bind/--port, ready line on stdout like the editor host [precedent docs/logging-and-editor.html ~line 299], --tokens-file, --max-clients, --relay RESERVED for C3, physics GPU forced off, no audio, no renderer), join tokens (line ~93), decisions N5, N11 (new executable, not a mode of N2EditorHost or lua_project), N12. Read the merged A1-A5 code first (n2net, n2security incl. TokenTable/join tokens, SecureTransport, UdpTransport, the A5 handshake) and how lua_project/the editor host start up and print their ready line. SCOPE: (1) new executable N2Server (CMake target, Linux + Windows, not in the web build): arguments --project, --scene, --bind, --port, --tokens-file, --max-clients, --log-*, same argument-parsing conventions as the other hosts; headless: renderer NONE, audio off, physics GPU forced off; loads the project and scene, runs the fixed-tick loop headless, listens on a SecureTransport over UdpTransport using the A5 handshake, FAIL-CLOSED bind rules like the editor host (refuse a non-loopback bind without a tokens file or explicit opt-in flag, mirroring the editor host's behaviour: read how it does it), prints the ready line on stdout once listening, clean shutdown on SIGINT/SIGTERM/console close; (2) the join-token generator CLI that A5 deferred to A6 (generate a token with a role player|spectator|admin, hash it like the editor's TokenTable, append to the tokens file; print the secret once, never log it; file permissions where the OS supports it); (3) at this stage the server accepts authenticated connections, enforces max-clients and the pre-auth/rate limits already in n2security, and logs connect/disconnect; NO replication or gameplay networking yet (that is track B); --relay is parsed but rejected as not implemented; (4) tests: unit tests for argument parsing and the token CLI; a smoke test that starts N2Server on loopback with a tokens file, waits for the ready line, connects a test client through the A5 handshake, then stops it, wired into ctest and the CI Linux leg (and Windows if the existing test harness runs executables there); (5) docs: a new docs/n2server.html (or a section in docs/netcode.html: pick what fits the docs layout) with flags, ready line, tokens, security notes, Linux service/Docker hints reusing the editor host's docs; update the docs/index.html design-decision table and docs/testing.html; verify every sentence against the code. Token handling is security-critical: never print or log secrets, compare in constant time (reuse TokenTable). Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^net/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/
- ^tools/
- ^n2server/
- ^apps/
- ^server/

## Acceptance
- N2Server builds on Linux and Windows, headless, ready line, clean shutdown
- Fail-closed bind and tokens file; join-token CLI prints the secret once and stores a hash
- Smoke test: start, handshake a test client, stop, in CI; --relay rejected as not implemented
- Docs verified against code; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security): fail-closed bind, no secret in logs/args/ready line, tokens file handling and permissions, constant-time compare via TokenTable, max-clients and pre-auth limits enforced, shutdown paths, no accidental listening on 0.0.0.0 by default.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a6-n2server` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-a6-n2server` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a6-n2server/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-a6-n2server --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a6-n2server/dev.json: {"pr":<number>,"branch":"feat-net-a6-n2server","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-a6-n2server` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
