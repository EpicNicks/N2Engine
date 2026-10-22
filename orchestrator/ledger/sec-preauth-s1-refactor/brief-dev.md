# Task sec-preauth-s1-refactor: Editor host pre-auth slots, PR S-1: per-connection state in a Connection struct, poll instead of select, no behaviour change (audit F1/F2/F6 groundwork)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-preauth-s1-connection-refactor.

## Issue / plan
Design (APPROVED by the user 2026-10-15): C:/Method Notes Obsidian/orchestrator/proposals/security-design-lua-editmode-and-preauth-slots.md PART 2 (read sections 2.1, 2.4, 2.6, 2.8 fully first), and the audit C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md findings F1, F2, F6. This item is ONLY the first stage, 'PR S-1, refactor with no behaviour change' of section 2.6: introduce a `Connection` struct that owns the per-connection state that is today single-threaded by construction (`_peer`, `_tlsSession`, `_helloTokenRefused`, the socket handle, accepted-at/connected-at times, authentication state: find every member of EditorServer that is per-connection state by reading ServerLoop/Send/Receive/WaitUntilReady/BeginTls and the Hello handling), move `Send`/`Receive`/`WaitUntilReady` into it, replace `select` by `poll` (`WSAPoll` on Windows) in the wait helpers (this also removes the FD_SETSIZE limit; keep semantics identical: timeouts, closed-peer detection, the injected connection clock and the TLS non-blocking send/write deadline from the merged #153), and move `ServiceTokens` (token file reload) to the accept loop IF that is possible without behaviour change (otherwise leave it and say why). The server STILL serves exactly one connection at a time on the same thread: there must be NO observable behaviour change: all existing suites (editor-server tests: ConnectionDeadlineTests, EditorTlsTests, RateLimitServerTests, EditorServerHardeningTests, host smoke tests, TLS smoke test, protocol golden vectors) must pass UNCHANGED (do not edit existing tests except where they reach into members you moved: say exactly which and why). Recent merges you must preserve (read them in git log first): #153 (connection deadlines: pre-Hello budget, TLS handshake 5 s own budget then 2 s Hello budget, idle/write/session deadlines, injected clock, non-blocking accepted sockets, SetAcceptedSocketHook/SetTlsHandshakeTimeout test seams), #132 rate limiting, #130 per-user tokens, #134 durable logs. Add tests only for the new unit seams (e.g. a Connection unit test with a socketpair-like loopback: Send/Receive/WaitUntilReady semantics, poll timeout, peer close). Think hard about what could silently change: partial reads/writes, EINTR/WSAEINTR, WSAPoll bugs (POLLHUP behaviour on Windows with connect failures, zero-fd edge), TLS record boundaries with poll vs select, socket close ordering, and the Windows-only differences: nothing builds locally, CI (10 checks) is the test and the Windows Debug TLS smoke test has been timing-sensitive, so keep timing identical. Docs: a short developer-facing note in docs/editor.html or logging-and-editor.html only if an existing section describes the select-based loop or members you moved (verify against code); do not claim anything about new threading (that is S-3). Unverified items: list honestly (Windows-specific poll behaviour, anything not covered by existing tests).

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^tests/editor-server/
- ^tests/common/
- ^docs/(editor|logging-and-editor|testing)\.html$

## Acceptance
- No behaviour change: all existing editor-server suites and smoke tests pass unchanged on all 10 checks
- Per-connection state lives in a Connection struct; Send/Receive/WaitUntilReady are Connection members using poll/WSAPoll
- New unit tests cover the Connection seams
- The server still serves one connection at a time on one thread
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: diff-level proof of no behaviour change: every moved member and every call site, timing semantics of poll vs select (timeout units, remaining-time recomputation, POLLIN|POLLHUP|POLLERR handling vs select readability, closed peer detection), TLS handshake/Hello deadline arithmetic from #153 untouched, injected clock usage preserved, hand-written Windows branches (WSAPoll, WSAEINTR), nothing left referencing removed members, tests unchanged except justified.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-preauth-s1-refactor` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-preauth-s1-connection-refactor` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-preauth-s1-refactor/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-preauth-s1-connection-refactor --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-preauth-s1-refactor/dev.json: {"pr":<number>,"branch":"sec-preauth-s1-connection-refactor","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-preauth-s1-connection-refactor` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
