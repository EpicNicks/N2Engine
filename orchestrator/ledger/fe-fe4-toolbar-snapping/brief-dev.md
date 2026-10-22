# Task fe-fe4-toolbar-snapping: Electron UI FE4: viewport toolbar (tool modes, local/world space, pivot, snapping toggles and increments) with prefs persistence and the shortcuts
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe4-toolbar-snapping.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md section 4 (Candidate 1: FE4 = viewport toolbar, modes, space/pivot, snapping settings, shortcuts), decisions U3 (shortcuts: W/E/R, Q select-only, X local/world, Z pivot/center) and U4 (snap defaults: translate 0.5, rotate 15 degrees, scale 0.1, RELATIVE (delta) snapping by default with an absolute-grid option for translate only, Ctrl inverts the snap toggle). FE3 (merged) put the tool mode on ViewportController with keymap actions gizmo.translate/rotate/scale/none and a text tool label, uses Ctrl snapping with constants (15 degrees, ratio step 0.1, move 0.5), world-axis rotate rings and local-axis scale handles: read FE3's code and README first (viewport-controller.ts, viewport-drag.ts, viewport-gizmo.ts, viewport-keys.ts, keymap.ts). SCOPE: (1) a viewport TOOLBAR (a registered part of the viewport panel, not a new panel) with: tool mode buttons (select/translate/rotate/scale, showing the active one, tooltips with the CURRENT chords from the keymap), space toggle (world/local) and pivot toggle (pivot/center) as the roadmap describes (implement what the roadmap lists for FE4; the translate gizmo's axes follow the space: local axes use the primary's rotation; rotate rings follow the space too if the roadmap says so), snap toggles per mode with increment inputs (translate, rotate, scale) and the relative/absolute translate option; Ctrl inverts the active snap during a drag; (2) keymap actions gizmo.space (X) and gizmo.pivot (Z) registered with their defaults (viewport scope), replacing the FE3 tool strip text label (keep it accessible); (3) settings persist through the F2 prefs store: user-scope for the toolbar choices per the roadmap (check whether snapping increments are user- or project-scope there), validated, safe to corrupt storage; (4) all maths for space/pivot/snapping in framework-free tested files: snap rounding with floating error, relative vs absolute translate, angle snap across wrap, scale ratio step, local-axis translate drags, pivot-center of a multi-selection (bounds centre via GetEntityBounds as the roadmap says), DOM tests for the toolbar (buttons, keyboard, persistence, disabled when the host is read-only), keymap tests; keep every existing test green; README viewport section updated and verified against the code. Frontend rules: requiresDocs is false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; NEVER run taskkill or kill other processes; keep changes inside the files your lane owns (other frontend items are in flight: README.md and keymap/panel registrations will collide mechanically: keep each as a small separate hunk; the repo README is LF); no engine/protocol change unless this item says so. Read the named design sections and the decisions (all ACCEPTED by the user) FIRST. Merged and usable: F1 panel registry/layout, F2 prefs store + keymap, FE3 rotate/scale gizmos (mode on ViewportController, W/E/R/Q keymap actions), FE5 inspector local transform, FE6/FE7 filters, FE8 history panel, MCP1 n2-mcp (mcp/), the protocol 1.14.0 typed client.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Viewport toolbar with tool modes, local/world space, pivot/center and snap toggles/increments (relative default, absolute translate option, Ctrl inverts)
- Keymap actions for space and pivot; prefs persistence; the FE3 label replaced accessibly
- Maths unit-tested; DOM tests; typecheck, build, tests and frontend CI green; README updated

## Notes
Reviewer: snap/space/pivot maths correctness and edge cases, drags still one edit group with cancel restoring, Ctrl inversion, read-only disabled, prefs validation, keymap integration, no regression of FE3 behaviour, tests meaningful, mediums count. Unverified: feel in a real viewport.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe4-toolbar-snapping` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe4-toolbar-snapping/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe4-toolbar-snapping --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe4-toolbar-snapping/dev.json: {"pr":<number>,"branch":"feat-fe-fe4-toolbar-snapping","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe4-toolbar-snapping` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
