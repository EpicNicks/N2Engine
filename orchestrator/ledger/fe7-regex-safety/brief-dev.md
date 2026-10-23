# Task fe7-regex-safety: FE7 follow-up: make the console regex filter safe (linear-time matching or a hard work cap) replacing the NestedRepeat heuristic
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: fix-fe7-regex-safety.

## Issue / plan
Follow-up to merged FE7 (frontend PR #38; src/renderer/console-filter.ts ~line 73). The user accepted the review low as a follow-up: the NestedRepeat ReDoS heuristic is bypassed by shapes such as ((a+))+ and (a|aa)+$, so a careless or hostile pattern typed into the console regex filter can hang the renderer. FIX properly (do not extend the heuristic): choose the safest approach that fits the repo and justify it in the README/dev.json: (a) run user regexes in a Web Worker (or a node-compatible equivalent in tests) with a hard time budget per filter pass and terminate+disable the regex mode on overrun with an inline message, or (b) a linear-time matcher (a small vetted RE2-style implementation as a dependency only if it is tiny, MIT/BSD, pinned and covered by notices; otherwise implement a restricted subset), or (c) a bounded-work wrapper that processes the lines in small chunks with a deadline between chunks (a single catastrophic match can still hang, so (c) alone is NOT acceptable). Keep the existing behaviour: regex off by default and never persisted as on, caps of 200 pattern characters and 4096 input characters, inline invalid-regex messages. TESTS: the old bypass patterns (((a+))+, (a|aa)+$, (a*)*b, (x+x+)+y) against long adversarial inputs must complete within the budget or be rejected without hanging the test (use a test timeout), normal patterns still match identically, the disable-on-overrun path, DOM test of the inline message; README updated. Frontend rules: requiresDocs is false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; NEVER run taskkill or kill other processes; keep changes inside the files your lane owns (other frontend items are in flight: README.md and keymap/panel registrations will collide mechanically: keep each as a small separate hunk; the repo README is LF); no engine/protocol change unless this item says so. Read the named design sections and the decisions (all ACCEPTED by the user) FIRST. Merged and usable: F1 panel registry/layout, F2 prefs store + keymap, FE3 rotate/scale gizmos (mode on ViewportController, W/E/R/Q keymap actions), FE5 inspector local transform, FE6/FE7 filters, FE8 history panel, MCP1 n2-mcp (mcp/), the protocol 1.14.0 typed client.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Catastrophic patterns can no longer hang the UI (worker with a hard time budget or a linear-time matcher); old bypass shapes tested
- Existing regex filter behaviour unchanged; tests and frontend CI green; README updated

## Notes
Reviewer: really cannot hang (the cap is enforced outside the matching thread, or the algorithm is linear), adversarial corpus meaningful, no leaked workers/timers, behaviour parity for normal patterns, dependency (if any) pinned and licensed, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe7-regex-safety` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe7-regex-safety/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-fe7-regex-safety --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe7-regex-safety/dev.json: {"pr":<number>,"branch":"fix-fe7-regex-safety","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-fe7-regex-safety` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
