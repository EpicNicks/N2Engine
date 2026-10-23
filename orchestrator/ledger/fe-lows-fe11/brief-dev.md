# Task fe-lows-fe11: Electron UI FE11 follow-ups: Remove-for-all uses the post-confirm component list; a refusal rolls back earlier edits
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: fix-fe11-lows.

## Issue / plan
Follow-up of merged frontend PR FE11 (#50, inspector multi-edit); the user accepted these LOW review findings as a follow-up. Verify each against the code on master first. (1) Remove-for-all uses the component list captured BEFORE the confirm dialog (src/renderer/inspector-multi.ts ~line 484): re-read the selection/component list after the dialog resolves (or abort if the selection changed) so a selection change while the dialog is open cannot remove from the wrong objects; add a test. (2) A refusal mid-batch leaves earlier objects edited with no rollback (inspector-multi.ts ~line 526; one Undo reverts today): on refusal, roll the already-applied edits back (undo the open edit group) when the protocol allows it, otherwise show a clear message that N objects were already changed and one Undo reverts; choose what the existing edit-group/undo protocol supports and describe it in the report; add a test. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test must pass before you push; CI has 3 checks. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package.json$
- ^package-lock.json$
- ^README.md$
- ^src/
- ^scripts/

## Acceptance
- Remove-for-all acts on the post-dialog component list or aborts on a changed selection
- A refusal mid-batch rolls back or clearly reports earlier edits; typecheck, tests and 3 CI checks green

## Notes
Reviewer: both findings fixed at the cited code with tests; no change to the single-object edit paths.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-lows-fe11` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-lows-fe11/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-fe11-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-lows-fe11/dev.json: {"pr":<number>,"branch":"fix-fe11-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-fe11-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
