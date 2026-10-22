# Task net-a5-lows: Netcode A5 follow-ups: SecureTransport drains the inner transport before its idle sweep; stale Disconnect wording in netcode.html
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-net-a5-lows.

## Issue / plan
Follow-up of merged A5 (#210) and A4 lows (#207); the opus delta review found two LOW issues, accepted as a follow-up. Verify each on master first. (1) engine/src/network/n2net/SecureTransport.cpp (~lines 544-553): SecureTransport::Poll runs RunTimers (idle sweep) BEFORE inner_->Poll drains the socket: the sweep-then-drain order that #207 removed from UdpTransport. A caller stall longer than idleTimeoutMicros ends established sessions although their authenticated packets are queued (fails closed, robustness only). Fix: drain the inner transport first (processing and authenticating events), then run the timers against the time the Poll began (as UdpTransport::Poll does); keep pre-auth/handshake timeout semantics unchanged; add a test with a stall longer than the idle timeout and authenticated datagrams queued: the session survives; a session that sent nothing is still ended. (2) docs/netcode.html, the udp-peers 'Disconnect is local' bullet: 'This differs from the interface's wording for SimTransport' is stale since the interface text says it depends on the transport: reword to be true against the code. Also check docs/netcode.html for any other sentence about the order of idle sweep and drain in SecureTransport and fix it. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- SecureTransport::Poll drains then sweeps; stall test passes
- netcode.html Disconnect wording true against the code
- CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: drain bounded as today, no authentication bypass introduced by the new order, test not flaky (deadline polling, no sleeps tuned to CI speed), docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a5-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-net-a5-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a5-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-net-a5-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a5-lows/dev.json: {"pr":<number>,"branch":"fix-net-a5-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-net-a5-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
