# Task sec-audit11-sanitised-errors-adminonly-luaseed: Security audit item 11 (F10), MCP0b: sanitised errors (no host paths or raw exception text to clients; res://-relative paths) and determinism.luaSeed in SetProjectSettings made admin-only
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-sec-audit11-sanitised-errors.

## Issue / plan
Security audit item 11 / finding F10 of C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md (read F10 and the table row 11) and C:/Method Notes Obsidian/orchestrator/proposals/mcp-engine-design.md (MCP0). The user accepted ALL recommendations on 2026-10-20. TWO PARTS. PART 1 (F10): error and success responses carry absolute host paths and raw exception text (ReadTextAsset, WritePlaySnapshot, SaveSceneToFile, asset commands, import errors, scene file errors, ...): map engine/filesystem exceptions to GENERIC messages for REMOTE (non-local) clients and keep the detail in the server log (admin-only after item 10: a sibling item 'sec-audit10-viewer-event-filtering' tags log lines; if its tagging API is not merged yet, log the detail with the existing logger and do not depend on it); return snapshot and asset paths as res://-relative (or .n2/play/<name>) when the client is not local; a LOCAL client (loopback/no tokens, the single local user) may keep detail: define 'local' precisely (reuse the host's existing loopback/identity notion) and test both. Find every response string that includes a path or e.what() (grep editor-server/src for e.what(), std::filesystem path to_string/generic_string in response builders, 'Failed to' messages) and sanitise each with a helper (one function, one place), keeping messages actionable (what failed, which res:// path, a stable reason like 'file not found', 'permission denied', 'invalid format'). PART 2: SetProjectSettings is a Write-class command, so an editor-role user can set determinism.luaSeed, which the engine itself flags as a security trade-off (see docs/scripting-lua.html determinism section and the SetProjectSettings handler): make a change to the determinism block (at minimum determinism.luaSeed; check whether other keys in that block or network/physics keys have the same class of risk and report, but only change luaSeed unless the code shows a stronger reason) REQUIRE the admin role, answering the existing forbidden error code for a non-admin with a clear message; the editor UI will adapt later (frontend item); document; add tests (editor role refused with forbidden and nothing written; admin allowed; viewer refused as today; a no-token loopback local user keeps today's behaviour: state and test). TESTS: golden-message tests for ReadTextAsset and WritePlaySnapshot failures (remote vs local), a table test that scans response builders for forbidden substrings (an absolute path prefix such as the project root or drive letter) using a temp project under an unusual absolute path, the luaSeed role tests. No protocol change (allowProtocol false: STOP and report if needed). DOCS: docs/logging-and-editor.html (error text and paths per client kind), docs/editor.html (roles: luaSeed admin-only), docs/scripting-lua.html determinism section, docs/testing.html. Verify every docs sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$

## Acceptance
- No absolute host paths or raw exception text reach remote clients in any response; res://-relative paths; detail kept in the server log; local client behaviour defined and tested
- determinism.luaSeed in SetProjectSettings requires admin (forbidden for editors), tested; other risky keys reported
- Golden-message tests and a response-path scan test; no protocol change
- All 10 checks green; docs verified against code
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: grep every response builder for paths and e.what() (nothing missed), one sanitising helper, the local-vs-remote definition is sound (a remote client cannot be classified local), luaSeed gate cannot be bypassed through another command (SetComponentFields, WriteTextAsset to project.n2proj, import settings?): check that project settings cannot be written some other way, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-audit11-sanitised-errors-adminonly-luaseed` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-sec-audit11-sanitised-errors` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-audit11-sanitised-errors-adminonly-luaseed/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-sec-audit11-sanitised-errors --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-audit11-sanitised-errors-adminonly-luaseed/dev.json: {"pr":<number>,"branch":"feat-sec-audit11-sanitised-errors","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-sec-audit11-sanitised-errors` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
