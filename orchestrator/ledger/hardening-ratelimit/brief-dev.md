# Task hardening-ratelimit: Exposed host hardening PR C: failed-Hello rate limiting and lockout (per-address backoff, loopback global backoff)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-hardening-ratelimit.

## Issue / plan
Design and the user's DECISIONS: C:/Method Notes Obsidian/orchestrator/proposals/exposed-host-hardening.md (read section 2b and 'Decisions (user, 2026-10-13)' fully first). PR A (#126) and PR B (per-user tokens, roles, protocol 1.12.0) are merged: re-read EditorServer.cpp (ServerLoop, BeginTls, HandleClient, HandleHello), HostOptions and the tokens code at current master first; line numbers in the proposal are stale. Scope: (1) read the peer address after accept (getpeername; IPv4 today, handle IPv6 sockaddr safely if the listener can produce it) and include `peer=<addr>` in the Hello success/failure log lines and the audit lines PR B added (user/role are already there); log lines must not include tokens. (2) A per-address failure table on the network thread: {failures, nextAllowed, lastSeen}, capped (about 4096 entries, evict the oldest) so it is not itself a memory DoS. Policy: after 3 failed Hellos from an address, backoff 1 s doubling to 5 min; reset on a successful Hello or after 15 minutes idle. A peer inside its backoff window is accepted and closed IMMEDIATELY, before the TLS handshake (the costly part), with one Info/Warn log line per ban start (not per rejected connection, to avoid log flooding; keep a suppressed-count). Prior offenders get a shorter 2 s TLS+Hello budget instead of 5 s. (3) NO trusted-proxy support (no PROXY protocol, no X-Forwarded-For). Loopback peers (127.0.0.0/8, ::1) are EXEMPT from per-address bans (an SSH tunnel or reverse proxy makes every peer 127.0.0.1) but subject to ONE global failed-Hello backoff for all loopback (for example 10 failures per minute across loopback, then the same doubling backoff keyed 'loopback'); and, because per-user tokens exist, also key a per-user backoff on the user NAMED in Hello (failures for a known user name from anywhere), bounded so an attacker cannot lock a real user out of a loopback/tunnel setup for long: make the user backoff short-capped (e.g. max 30 s) and document the tradeoff. A host with no authentication configured has no failed-Hello concept: no table, no behaviour change. (4) Use an injected clock (a small Clock interface or std::function now()) so tests NEVER sleep; the table logic lives in a separate unit (e.g. RateLimiter.hpp/.cpp) tested directly: backoff doubling and cap, reset on success, reset on idle, eviction at the cap, loopback global rule, per-user rule, short budget for prior offenders. Plus socket-level tests with a fake peer-address seam (an injectable address provider) proving a banned peer is closed before TLS/Hello and a good user from another address is unaffected. (5) Options: add HostOptions flags only if needed (e.g. `--no-rate-limit` for test rigs, off by default being rate limiting ON when authentication is configured); keep defaults as above and document them. NO protocol change. (6) Docs: docs/editor.html and docs/logging-and-editor.html (policy table, loopback behaviour, what it cannot do: the serial-server starvation window remains, the pre-auth accept thread is deferred; firewall allow-listing stays the primary control; the log lines). Do NOT do deploy/Docker/systemd changes (PR D). Do NOT change the frontend repo. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`).

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/
- ^cmake/
- ^CMakeLists\.txt$

## Acceptance
- All Tests-workflow jobs green
- Failed-Hello backoff per address with injected-clock tests that never sleep
- Loopback exempt from per-address bans but under one global backoff; banned peers closed before TLS
- No change for a host without authentication; no protocol change
- Docs describe the policy and its limits
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive: reviewer checks a banned peer cannot cause work before the close, the table is bounded, no lockout of a legitimate loopback/tunnel user beyond the documented cap, no log flooding, injected clock everywhere (no sleeps in tests), thread safety (network thread only). Unverified: real source addresses behind NAT/LB/tunnel, behaviour under real latency.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs hardening-ratelimit` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-hardening-ratelimit` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/hardening-ratelimit/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-hardening-ratelimit --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/hardening-ratelimit/dev.json: {"pr":<number>,"branch":"feat-hardening-ratelimit","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-hardening-ratelimit` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
