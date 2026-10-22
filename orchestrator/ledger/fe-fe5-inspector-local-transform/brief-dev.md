# Task fe-fe5-inspector-local-transform: Electron UI FE5: inspector transform row switches to the local transform (GetEntity + SetLocalTransform) so it matches the gizmo, with Euler display and multi-object editing of shared fields
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe5-inspector-transform.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md (section 4 for the inspector transform part: FE5; and the multi-edit part of section 11 'multi-select (what remains)' for multi-edit in the inspector), decision U5 (switch the inspector's transform row to the LOCAL transform: GetEntity + SetLocalTransform, Euler shown, quaternion sent, so it matches the gizmo; today the row uses the legacy world Euler commands). Read src/renderer/inspector-panel.tsx, inspector-editors.tsx, inspector-fields.ts, inspector-state.ts, the transform editor, how the gizmo writes the local transform (viewport-controller.ts) and the engine docs for GetEntity/SetLocalTransform (engine repo docs/editor.html READ-ONLY) FIRST. SCOPE: (1) the transform row shows and edits LOCAL position/rotation/scale: rotation displayed as Euler degrees (decide the order/convention from the engine docs and the gizmo's maths, and document it), edits converted to a quaternion and sent with SetLocalTransform in one edit step per commit (typing then Enter/blur), undo works, values stay stable on round trips (no drift from Euler<->quaternion conversions: preserve the last edited Euler triple while editing, gimbal cases tested), live refresh when the gizmo or another client changes the object, world values may be shown read-only as a tooltip/secondary row only if cheap; (2) MULTI-EDIT: when several objects are selected the inspector shows shared components' fields with mixed-value indicators; editing a field applies to ALL selected objects as ONE edit group (one undo step), only for field types the roadmap lists, with a clear 'N objects' header, read-only/viewer respected, partial failure handling (a failed object rolls the group back or reports, per the roadmap's decision: follow it), (3) tests: pure Euler/quaternion helpers (round trips, gimbal, negative zero, large angles, wrap), inspector state for mixed values, DOM tests for transform edit/undo, multi-edit group count with a fake backend, read-only; README updated. Frontend rules: requiresDocs false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; keep changes inside the files your lane owns (other frontend lanes are in flight at the same time: keymap action registrations and README additions will conflict mechanically, keep them as small separate hunks); no engine/protocol change unless this item says so. Read the roadmap section named in this item, decisions U1-U21 (all ACCEPTED by the user on 2026-10-20), the frontend README and the code you touch FIRST; F1 (panel registry/layout) and F2 (prefs store and keymap) are merged: use them (register panels through the registry, shortcuts through the keymap, preferences through the prefs store).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Inspector transform row edits the local transform consistently with the gizmo, stable Euler display, one edit step per commit
- Multi-object editing of shared fields with mixed-value indicators as one edit group; read-only respected
- Unit and DOM tests; frontend CI green; README updated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: Euler/quaternion conversion correctness and stability (no drift while editing), local vs world semantics identical to the gizmo, one edit group per commit/multi-edit, mixed-value handling, partial-failure behaviour, read-only, tests meaningful, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe5-inspector-local-transform` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe5-inspector-local-transform/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe5-inspector-transform --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe5-inspector-local-transform/dev.json: {"pr":<number>,"branch":"feat-fe-fe5-inspector-transform","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe5-inspector-transform` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
