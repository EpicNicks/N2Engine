# Task fe-lows-fe9b-fe10: Electron UI follow-ups: no-op Apply on an invalid stored layer matrix, import-settings draft/edited fixes
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: fix-fe9b-fe10-lows.

## Issue / plan
Follow-up of merged frontend PRs FE9b (#48) and FE10 (#49); the user accepted these LOW review findings as a follow-up. Verify each against the code on master before changing it. (1) FE9b: with an invalid stored collision-layer matrix, a no-op Apply leaves it invalid (src/renderer/project-settings-schema.ts ~line 516): Apply with unchanged values must not leave an invalid matrix in place silently: either normalise/repair it on Apply or keep Apply blocked with a clear message, whichever fits the existing validation flow; add a test. (2) FE10: an invalid number draft keeps Apply blocked after switching to the JSON view (assets-panel.tsx ~line 263): switching view must discard or surface the invalid draft so Apply state matches what is shown; add a DOM test. (3) FE10: "1.0" versus "1" counts as edited (assets-state.ts ~line 364): compare numbers by value, not by text; add a test. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test must pass before you push; CI has 3 checks. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package.json$
- ^package-lock.json$
- ^README.md$
- ^src/
- ^scripts/

## Acceptance
- No-op Apply never silently keeps an invalid layer matrix
- Invalid number draft does not block Apply after switching to JSON view
- 1.0 vs 1 is not an edit; typecheck, tests and 3 CI checks green

## Notes
Reviewer: each of the three findings fixed at the cited code with a test; no behaviour change elsewhere in the settings/assets panels.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-lows-fe9b-fe10` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-lows-fe9b-fe10/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-fe9b-fe10-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-lows-fe9b-fe10/dev.json: {"pr":<number>,"branch":"fix-fe9b-fe10-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-fe9b-fe10-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
