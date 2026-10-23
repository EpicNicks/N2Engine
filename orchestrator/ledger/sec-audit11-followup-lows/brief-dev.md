# Task sec-audit11-followup-lows: Audit 11 follow-ups: path redaction left-boundary bypass after ":" or "." , escaped and URL-encoded separators, and the docs line about the working folder
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-sec-audit11-followup-lows.

## Issue / plan
Follow-up to merged #192 (editor-server/src/ClientText.cpp, tests/editor-server/ClientTextTests.cpp, docs/logging-and-editor.html 'net under the messages'; C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md F10). The user accepted these as follow-ups. Read the #192 review in C:/Method Notes Obsidian/orchestrator/ledger/sec-audit11-sanitised-errors-adminonly-luaseed/review.json first. FIX: (1) StartsInsideName treats a host root right after ':' or '.' (or a slash run that is not file:// or the Windows long-path prefix) as 'inside a name' and leaves it unredacted, so 'open:C:\\proj\\assets\\a.txt' or 'key:/srv/game/x' can reach a remote client: tighten the rule so it only protects a root that is a path SEGMENT of the client's OWN res:// or user:// path (preceded by 'res://' or 'user://' + name chars etc.), and redacts a real host root after ':' '.' '=' quotes brackets and spaces; keep the earlier fix (a root is never replaced inside a client's own res:// path) with its test; (2) normalise escaped separators before matching: doubled backslashes (JSON-escaped 'C:\\\\proj'), forward/back mixed, and percent-encoded separators/roots (%5C, %2F, %3A) so the safety net matches them; redact to the same labels; (3) docs/logging-and-editor.html: the safety-net section must list exactly the roots the code uses (working folder only when it is not a drive/file-system root) and describe the boundary rule; (4) TESTS: negative controls for each bypass above (they must fail on the old code), the earlier left-boundary and drive-root tests stay green, a property-style test that no configured root survives in output for a corpus of wrappers (quotes, brackets, 'in X', 'file X', JSON-escaped, URL-encoded). Comment/code only inside ClientText.* and its tests plus the docs. Nothing builds locally for the engine; CI (11 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^tests/
- ^docs/

## Acceptance
- Real host roots are redacted after ":" "." "=" quotes and brackets; only the client's own res://user:// path segments are protected
- Doubled/mixed/percent-encoded separators matched
- Negative-control tests fail on the old code; docs true; all checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: try to bypass the redaction with realistic wrappers; the protection of a client's own res:// path is not abused to hide a host root; normalisation cannot turn client text into a false positive that corrupts valid client paths; tests meaningful; mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-audit11-followup-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-sec-audit11-followup-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-audit11-followup-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-sec-audit11-followup-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-audit11-followup-lows/dev.json: {"pr":<number>,"branch":"fix-sec-audit11-followup-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-sec-audit11-followup-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
