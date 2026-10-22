# Task net-b7-rpcs: Netcode B7: RPCs declared in C++ and Lua, budgets and rate limits
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b7-rpcs.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read the whole document, the sections for this stage and the stage table row, then the MERGED code: n2net (transports, SecureTransport, channels, the network simulator and its tests), N2Server, B1 (NetworkObject, world/server/client classes, fingerprint) and B2 (field replication: .Replicated(), codecs, delta vs acked baseline, NetworkTransform) in engine/src/network, A2 (simulation clock, fixed tick, StepTicks, TickOptions), and the existing network docs page. Follow the security rules of the design (bounded counts and sizes before allocation, rate limits, authority checks, no client-controlled allocation). The web build must still compile (n2net core is compiled for web; native sockets are not). Nothing builds locally: re-read every edit twice and trace identifiers. SCOPE (stage B7 only): (1) RPC declaration: C++ (a registration API next to component registration with a stable id, direction ServerToClient / ClientToServer / ServerToOwner, reliable or unreliable, an argument schema built from the FieldKind codecs of B2) and Lua (declared in the component declaration table the same way fields are, with params typed by the same kind rules as GetLuaFields); RPC ids are part of the fingerprint from B1/B2; (2) sending: calling the RPC on the sending side encodes and queues it on the right channel (reliable ordered or unreliable), with the target (all, one client, owner); receiving: decode with bounds, validate the argument values with ValidateFieldValue/ClampFieldValue, check direction and authority (a client can call only ClientToServer RPCs on objects it owns/controls; the server never executes a ServerToClient RPC received from a client), invoke the handler in the deterministic tick order; (3) budgets and rate limits: per-client RPC per second and byte budgets per RPC id and in total, queue bounds, over-budget RPCs dropped with counters and a throttled log (never unbounded), handler time budget through the existing Lua budget mechanism, a faulting handler does not kill the server; (4) tests on the simulator and loopback: delivery semantics (reliable ordered vs unreliable), authority rejection, argument validation, rate limiting, malformed input, Lua and C++ RPCs; (5) docs true against the code. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^net/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/
- ^tools/
- ^n2server/
- ^apps/
- ^server/
- ^lua_project/
- ^[^/]*\.lua$

## Acceptance
- RPC declaration in C++ and Lua with ids in the fingerprint
- Direction/authority checks, validated arguments, bounded decode
- Per-client budgets and rate limits with counters; faulting handler isolated
- Simulator/loopback tests, docs true, CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security): authority/direction enforced on the receiving side for every path, argument validation before invoking Lua/C++, no unbounded queues, rate limit state bounded per client, reliable queue cannot exhaust memory, handler faults isolated, ids collision/ordering, compile-level correctness, web build.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b7-rpcs` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b7-rpcs` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b7-rpcs/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b7-rpcs --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b7-rpcs/dev.json: {"pr":<number>,"branch":"feat-net-b7-rpcs","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b7-rpcs` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
