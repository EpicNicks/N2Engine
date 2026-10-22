# Task net-b1-network-object: Netcode B1: NetworkObject component, net ids, prefab table, scene-object matching, spawn/despawn replication, scene and build fingerprint check
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b1-network-object.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (N1-N15 ACCEPTED): read the whole document, in particular the engine layer description (NetworkObject component, field replication through reflection), the server-authoritative runtime sections, the scene/prefab/fingerprint paragraphs, row B1 of the stage table (line ~296) and the decisions N1-N15. Track A is merged: read the merged code first (engine/src/network and n2net: transports, SecureTransport handshake, join tokens; the N2Server executable in n2server/ or wherever A6 put it; A1 presets/settings; A2 simulation clock and StepTicks; ApplicationOptions; scene/prefab/serialization code under engine/src; ComponentRegistry and reflection). SCOPE (B1 only, NO field replication yet: that is B2): (1) NetworkObject component (registered like other components, serializable, with net id assignment authority on the server, an owner/authority field consistent with the design, a prefab key or scene-object key) and the NetworkId type; (2) server-side world replication of object lifetime: the server assigns net ids to NetworkObjects at scene load (scene objects matched by a stable key so client and server agree) and at runtime spawns/despawns (prefab table by name/hash); clients receive Spawn/Despawn messages over the existing secure channel and instantiate/destroy the local objects; a message codec with versioned wire format and fuzz/size-bounded parsing (all lengths capped, malformed input rejected, no allocation driven by untrusted counts); (3) the scene + build fingerprint check at join: the server sends its fingerprint (scene content hash + engine/protocol version + registered component/prefab table hash) and the client refuses to join on mismatch with a clear error, as the design says; (4) N2Server wires B1 in (server hosts the world and replicates spawns) and a minimal client-side NetworkClient runtime hook (headless-capable) so a loopback integration test can connect a client to N2Server over real loopback UDP with the secure handshake and observe spawn/despawn and the fingerprint refusal; use the existing network simulator for unit tests where possible; (5) tests: codec round trip and malformed-input rejection tests, id assignment and scene matching tests, spawn/despawn tests on the simulator, the multi-process loopback integration test registered in ctest and CI like the A6 smoke test; (6) docs: a new section/page per the proposal's docs plan (docs/netcode.html or networking.html: pick what the A6 docs used) verified against the code. Respect every security rule from the design (bounded counts, rate limits, authority checks: a client can never spawn or despawn). Do not break the Web build (n2net core is compiled for web; native sockets are not). Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

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

## Acceptance
- NetworkObject with net ids, scene-object matching and prefab spawn/despawn replication
- Bounded versioned codec; malformed input rejected; client can never spawn/despawn
- Fingerprint mismatch refuses the join with a clear error
- Loopback integration test with N2Server in CI; docs true; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security/protocol): every length and count bounded before allocation, authority checks on every message type, fingerprint covers what the design says, id reuse/wraparound safe (no stale id confusion), codec versioning, no UB on malformed input, web build still compiles, integration test not flaky (ready-line wait, loopback ports).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b1-network-object` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b1-network-object` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b1-network-object/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b1-network-object --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b1-network-object/dev.json: {"pr":<number>,"branch":"feat-net-b1-network-object","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b1-network-object` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
