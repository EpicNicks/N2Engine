# Task net-a6-lows: N2Server follow-ups: orderly shutdown on the exception path, accurate stdout/Debug and token-check docs, a PeerConnected-without-identity session really dropped
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-net-a6-lows.

## Issue / plan
Follow-up of merged A6 (#216, the N2Server executable); the opus review found four LOW issues that the user accepted as a follow-up. Verify each on master first (find the files with git grep N2Server / n2server). (1) The std::exception catch in N2Server's main returns 1 without OnApplicationQuit or app.Shutdown(), so scenes, physics and audio are torn down only by static destruction, which can crash or hang: run the same orderly shutdown path as the normal exit (guarded, never throwing), keep exit code 1; add a test if the structure allows (an injectable failure) else explain. (2) The docs claim 'nothing else is written to stdout' but in Debug builds Application::Init calls Logger::InitializeDebugConsoleHelper (N2ENGINE_DEBUG) which echoes log events to the console: either make the server suppress that helper so the claim is true (preferred: the ready line must stay the only stdout line a supervisor needs to parse, check how the editor host handles it) or correct the sentence precisely; test or docs verified against the code. (3) The docs Security section says join proofs are matched with BasicTokenTable::Authenticate but SecureTransport::OnConnectResponse runs its own constant-time HMAC loop: reword to what the code does (still constant time, still the same tokens file semantics) after reading both. (4) When SecureTransport reports PeerConnected without an identity, the server logs that the session 'is dropped' but nothing disconnects it (SecureTransport::Disconnect is a no-op inside Poll's handler): make the server really drop it (e.g. queue the disconnect and apply after Poll returns, mirroring SecureTransport's deferredInnerDisconnects_ pattern) and add a test, or if the situation cannot occur in practice change the log text to say so and justify it in the report with the code path. Security-relevant: keep every existing fail-closed rule; no secret in logs. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

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
- Exception path shuts down in order with exit code 1
- stdout/Debug claim true; token-check docs true against the code
- A PeerConnected without identity is really disconnected (test) or the text justified
- CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: shutdown path cannot throw or deadlock, no secret printed, the drop really happens and cannot be skipped, no change to the bind/token rules, docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-a6-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-net-a6-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-a6-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-net-a6-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-a6-lows/dev.json: {"pr":<number>,"branch":"fix-net-a6-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-net-a6-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
