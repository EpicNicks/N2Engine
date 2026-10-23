# Task net-a4-lows: Netcode A4 follow-ups: drain the socket before the idle-peer sweep, DualStack() only for wildcard IPv6 binds, ITransport header text brought up to date
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-net-a4-lows.

## Issue / plan
Follow-up of the merged A4 PR #203 (native UdpTransport). The user accepted three LOW review findings as a follow-up item; verify each against the CODE on master before changing it: (1) engine/src/network/n2net/UdpTransport.cpp (~line 832): the idle-peer sweep runs BEFORE the socket is drained in Poll, so a stall between Polls longer than peerIdleTimeoutMs drops every peer although their datagrams sit in the receive buffer: drain first (bounded as today), then sweep, so a peer whose datagram was just received is not timed out; add a test that stalls longer than the idle timeout with datagrams queued and asserts the peer survives (and a peer that sent nothing is still swept); (2) UdpTransport.cpp (~line 613): DualStack() returns true for any IPv6 bind whose IPV6_V6ONLY=0 was accepted, including a specific address such as ::1 that can never reach IPv4, so Connect to an IPv4 address succeeds and every Send fails silently: DualStack() is true only for the IPv6 wildcard (::) bind; Connect to an IPv4 peer on a non-dual-stack IPv6 socket fails with a clear error; test both (skip cleanly when IPv6 is unavailable, as the existing tests do); (3) engine/include/engine/network/n2net/Transport.hpp (~line 52): the ITransport header comment still says the library has no sockets / native UDP is later work and that Disconnect makes the other side see PeerDisconnected; rewrite it to describe the current contract (UdpTransport exists, native only; the Disconnect semantics UdpTransport actually has, which the user accepted as a deviation in #203: read the A4 ledger C:/Method Notes Obsidian/orchestrator/ledger/net-a4-udp-transport/pr-body.md for the accepted wording). Update docs/netcode.html where it describes these behaviours. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- Poll drains the socket before the idle sweep; stall test passes
- DualStack() only for the wildcard IPv6 bind; IPv4 connect on a specific IPv6 bind is refused clearly; tested
- Transport.hpp comment matches the code
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: drain bound kept (no unbounded loop), sweep order, the IPv6 test skips cleanly, header text true, no flaky timing (deadline polling).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a4-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-net-a4-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a4-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-net-a4-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a4-lows/dev.json: {"pr":<number>,"branch":"fix-net-a4-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-net-a4-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
