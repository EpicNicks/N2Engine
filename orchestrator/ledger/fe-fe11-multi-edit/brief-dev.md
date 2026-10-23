# Task fe-fe11-multi-edit: Electron UI FE11: inspector multi-edit for several selected objects in one edit group
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe11-multi-edit.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md (U1-U21 ACCEPTED): section 11 (the Inspector multi-edit (FE11) paragraph, including what it says about Lua components) FULLY and section 16 (inspector lane: FE5 -> FE11; FE5 merged). Read the inspector files (inspector-*.ts(x)), scene-state.ts, the edit-group handling in edit-controller.ts and the FE8 history panel FIRST. SCOPE as specified: with several objects selected show the common header fields (active, tag, layer, transform) and the component types present on ALL of them (matched by type, first of each type); mixed values show a dash; an edit sends SetComponentFields / SetEntityProperties / SetLocalTransform per object inside ONE edit group labelled 'Edit N objects' (one undo step; on any failure the group is aborted/rolled back the way the existing edit groups do it); Lua components per the section; role/read-only gating; Node + DOM tests (mixed display, one undo step, partial failure, selection change while editing). Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test (Node + DOM tests) must pass before you push; CI has 3 checks. Append ONE README.md section for the feature (other PRs also append: keep yours self-contained). Follow the existing panel/keymap/prefs registries (F1/F2) and lane file boundaries in the roadmap section 16. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Common fields and shared component types shown with mixed-value display
- Edits applied to all objects in one edit group (one undo step), failure handled
- Node + DOM tests pass; 3 CI checks green

## Notes
Reviewer: edit-group open/close on every path (no group left open: the host refuses extension invokes while a group is open), partial failure, stale selection.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe11-multi-edit` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe11-multi-edit/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe11-multi-edit --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe11-multi-edit/dev.json: {"pr":<number>,"branch":"feat-fe11-multi-edit","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe11-multi-edit` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
