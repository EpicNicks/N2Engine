# Task ci-tls-smoke-warmup: Smoke test harness: build and warm up the TLS client context before connecting (fixes the intermittent Windows Debug TLS smoke failure)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: ci-tls-smoke-warmup.

## Issue / plan
ROOT CAUSE (investigated, read-only, by an agent that read the harness, the host code and the logs of 11 passing and 2 failing Windows runs): the intermittent failure `SMOKE TEST FAILED: ConnectionAbortedError [WinError 10053]` with the host log `Closing an editor connection: its TLS handshake didn't finish within 5000 ms` in the job `windows (Debug, PhysX OFF, FreeType ON, ufbx ON, TLS ON)` step `Editor host TLS smoke test` is a TEST HARNESS problem, not a host bug: tests/smoke/n2_frames.py:101 connects first and only afterwards (lines ~110-114) builds the ssl.SSLContext and calls wrap_socket, so the Python process's FIRST TLS handshake (context construction, RNG seeding, algorithm fetches, key-share generation: on the runner's Python 3.12.10/OpenSSL 3.0 this costs 0.15 s to 8.6 s of CPU-bound work, round 1 always slow, round 2 7-14 ms) falls inside the host's 5 s handshake budget, which starts at accept. The host is behaving as designed (it times out a client that is silent for 5 s after connecting); a real editor client (Node tls.connect) prepares its TLS state before connecting. DO NOT raise the host's budget (that would only hide it). FIX (test harness only): in tests/smoke/n2_frames.py `connect` (and any other place in tests/smoke/*.py that connects to a TLS host): (1) build the SSLContext BEFORE socket.create_connection (once at module level or once per process, cached); (2) warm it up before connecting with an in-memory handshake: `ctx.wrap_bio(ssl.MemoryBIO(), ssl.MemoryBIO(), server_hostname=<the same server_hostname the real connection uses>).do_handshake()` inside try/except ssl.SSLWantReadError (this generates a ClientHello in memory, paying the first-use costs up front); make sure the warm-up cannot change the verification behaviour of the real connection (the real connection must still pin/verify the certificate fingerprint exactly as today: read the file to see how verification is done, and do not weaken it; the warm-up context may be a separate or the same context, but the warm-up object must be discarded); (3) print (to stdout) how long the real handshake took (ms) in the tls scenario, so future runs show the number; (4) keep Linux/macOS behaviour working and the tls and plain scenarios otherwise unchanged; (5) re-read the Python for syntax; if the file's line endings are CRLF/LF keep them. No engine/host change, no workflow change. Nothing can be run locally for this Windows-specific step: the PR's CI (10 checks, esp. the Windows Debug TLS leg and the Linux required leg) is the test: say that the flake is intermittent so a single green run is weak evidence; the proof is the printed handshake timings in the logs plus the absence of failures over the next runs. Add a short note to docs/testing.html only if an existing paragraph describes the TLS smoke test and would now be wrong.

## Allowed paths (regex, repo-relative)
- ^tests/smoke/
- ^docs/testing\.html$

## Acceptance
- The TLS client context is built and warmed up before the TCP connect; certificate verification unchanged
- The tls scenario prints the real handshake duration in ms
- All 10 checks green; no host or workflow change

## Notes
Reviewer: certificate pinning/verification strictness unchanged (diff the verification code), warm-up uses no network and cannot throw uncaught, context created before connect, other smoke scenarios untouched, Python syntax and Windows paths.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs ci-tls-smoke-warmup` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref ci-tls-smoke-warmup` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/ci-tls-smoke-warmup/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head ci-tls-smoke-warmup --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/ci-tls-smoke-warmup/dev.json: {"pr":<number>,"branch":"ci-tls-smoke-warmup","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/ci-tls-smoke-warmup` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
