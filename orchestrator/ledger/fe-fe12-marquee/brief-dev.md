# Task fe-fe12-marquee: Electron UI FE12: marquee selection in the viewport
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe12-marquee.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md (U1-U21 ACCEPTED): section 11 (multi-select: the Marquee select (FE12) paragraph) FULLY and section 16 (viewport lane: FE3 -> FE4 -> FE12 -> FE13a -> FE13b; FE3/FE4 merged). Read the viewport files (viewport-*.ts(x)), the FE4 toolbar/modes, selection handling in the store, and GetEntityBounds in the engine protocol (cap 4096 ids per call) FIRST. SCOPE as specified: left-drag on empty space past the click slop (not on a gizmo handle) draws a rectangle; on release select objects whose projected world AABB intersects the rectangle (Alt: fully inside); Shift/Ctrl add/remove; bounds from GetEntityBounds for the hierarchy's nodes cached per scene revision and chunked above 4096; Escape cancels; no conflict with camera drags or gizmo drags; Node tests for the projection/intersection maths and DOM tests for the drag flow and modifiers. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test (Node + DOM tests) must pass before you push; CI has 3 checks. Append ONE README.md section for the feature (other PRs also append: keep yours self-contained). Follow the existing panel/keymap/prefs registries (F1/F2) and lane file boundaries in the roadmap section 16. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Marquee select with intersect/inside and add/remove modifiers
- Bounds cached per revision and chunked at 4096
- Node + DOM tests pass; 3 CI checks green

## Notes
Reviewer: projection maths (behind-camera points, clipping), chunking, no interference with gizmo/camera drags, cache invalidation.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe12-marquee` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe12-marquee/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe12-marquee --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe12-marquee/dev.json: {"pr":<number>,"branch":"feat-fe12-marquee","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe12-marquee` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
