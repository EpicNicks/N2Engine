# Task fe-fe3-rotate-scale-gizmos: Electron UI FE3: rotate and scale gizmos (maths, drawing, hit-testing, drags as one undo step), plus the two F2 lows
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe3-gizmos.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md section 4 (Candidate 1: rotate and scale gizmos, snapping, local/world, shortcuts; PR FE3 = the rotate/scale maths and drags; FE4 (toolbar, snapping settings, shortcuts) is the NEXT item: do not build the toolbar or snapping UI), decisions U3 (W/E/R shortcuts: you may register the actions gizmo.translate/rotate/scale in the keymap with default chords W/E/R and Q for select-only ONLY if the existing translate gizmo has a mode concept; otherwise expose a mode on the viewport controller and register the actions: verify against the code), U4 (snap defaults are FE4's), U5 (inspector local transform is FE5). Read the existing translate gizmo and its pure maths first: src/renderer/viewport-gizmo.ts, viewport-controller.ts, viewport-math.ts, viewport-input.ts, viewport-selection.ts, edit-controller.ts and edit groups, README viewport section. SCOPE: (1) rotate gizmo: three axis rings (world axes by default; local axes only if the roadmap says FE3 includes the space toggle: check) drawn on the overlay canvas at constant screen size with front/back fading, hit-testing by ring distance with a pick tolerance, dragging computes an angle delta around the axis from the pointer motion in the plane (stable at grazing angles: fall back to the screen-space tangent when the ring plane is edge-on), applies to the selected objects (the gizmo moves the whole selection as one undo step today for translate: do the same, rotating about the shared pivot per the roadmap or each object about its own pivot as the roadmap specifies), sends through the same SetLocalTransform/edit-group path as translate (ONE undo step per drag, cancel on Escape restores), (2) scale gizmo: three axis handles with end cubes plus a uniform-scale centre handle, drag math as a ratio from the pointer projection onto the axis (clamped away from zero and negative flips per the roadmap), applied as local scale with the same undo/cancel behaviour, (3) a mode on the controller (translate|rotate|scale), keyboard actions through the keymap, the current mode shown in the viewport (small text label until FE4's toolbar), (4) all maths in framework-free unit-tested files (ring hit/drag angle, scale ratio, constant-size, plane fallbacks, quaternion composition and Euler display consistency), DOM tests for mode switching, drag lifecycle (down/move/up/cancel) with a fake edit backend asserting exactly one edit group and the right transform commands, multi-selection, locked/read-only hosts (no drags when canWrite is false), (5) the TWO F2 LOWS: viewport.frame is a code chord (KeyF) but also matches any key whose e.key is 'f' (the old code was physical-key only and the README says so): make the matching physical-key only for code chords, test with a non-Latin layout simulation; bind() accepts chord lists that the KeymapOverridesPref validator rejects (more than 8 chords) and silently drops them: make bind() enforce the same limit and report the error, test, (6) README viewport section updated and verified. Frontend rules: requiresDocs false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; keep changes inside the files your lane owns (other frontend lanes are in flight at the same time: keymap action registrations and README additions will conflict mechanically, keep them as small separate hunks); no engine/protocol change unless this item says so. Read the roadmap section named in this item, decisions U1-U21 (all ACCEPTED by the user on 2026-10-20), the frontend README and the code you touch FIRST; F1 (panel registry/layout) and F2 (prefs store and keymap) are merged: use them (register panels through the registry, shortcuts through the keymap, preferences through the prefs store).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Rotate and scale gizmos with correct maths, constant screen size, edge-on fallbacks, one undo step per drag, Escape cancels, read-only respected
- Mode switch via keymap actions and a viewport label; no toolbar/snapping yet
- The two F2 lows fixed with tests
- Unit + DOM tests; typecheck, build, tests and frontend CI green; README updated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: gizmo maths correct (angle wrap, grazing planes, pivot semantics, degenerate scale), exactly one edit group per drag, cancel restores exactly, no frame request leaks, keymap integration (scopes, text entry), read-only/viewer, F2 lows really fixed, tests meaningful, README true, mediums count. Unverified: feel in a real viewport.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe3-rotate-scale-gizmos` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe3-rotate-scale-gizmos/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe3-gizmos --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe3-rotate-scale-gizmos/dev.json: {"pr":<number>,"branch":"feat-fe-fe3-gizmos","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe3-gizmos` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
