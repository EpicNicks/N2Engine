# Task fe-fe9-project-settings: Electron UI FE9: project settings panel over GetProjectInfo/SetProjectSettings/SetStartupScene (General, Physics, Rendering, Math, Determinism, Layers, Input as JSON, Raw JSON) with slots for Network and Editor Extensions
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe9-project-settings.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md section 5 (Candidate 2: project settings panel; FE9 and FE9b layers matrix), decision U6 (v1 scope: General/startup scene, Physics, Rendering, Math, Determinism (with restart badges and the luaSeed warning), Layers, Input as JSON, Raw JSON; Network (B11) and Editor Extensions (E-X4) added LATER as sections of the same panel: design the section registry so they slot in without touching existing sections). Read the engine docs for the settings the host exposes (engine repo C:/Method Notes Obsidian/N2Engine, READ-ONLY: docs/editor.html (GetProjectInfo, SetProjectSettings, SetStartupScene), docs/application.html (project settings Init reads: determinism.luaSeed, math.simdTier, physics.gpu, physics.deterministic, physics.backend, the network block), docs/physics.html, the settings parsing in engine/src/ProjectSettings.cpp for ranges and warnings), the protocol client (src/protocol), and the panel registry (register as a closable panel through F1, default group per the roadmap), prefs, keymap. IMPORTANT security fact (audit item 11, merged): the whole determinism block in SetProjectSettings is ADMIN-ONLY on token hosts (an editor gets forbidden): the Determinism section must be disabled with an explanation for non-admin roles (use the role from ServerInfo) and handle the forbidden error. SCOPE: a panel with a left list of sections and a form per section generated from a typed schema table in the client (field label, type, range, default, help, 'applies after restart' badge where the engine reads the value only at Init: derive from the engine docs; physics.* and determinism.* and math.simdTier are Init-time), values read with GetProjectInfo, edits sent with SetProjectSettings as minimal patches (only changed keys), validation before send mirroring the engine's ranges (the host still validates and warns: show host warnings/errors returned), the luaSeed warning text about the security trade-off, the startup scene picker over the scene list with SetStartupScene, a Layers section (names and the collision matrix per the roadmap FE9b only if the host exposes it; otherwise a read-only note), Input as a JSON editor with validation, Raw JSON section (read-only unless admin?: follow the roadmap) with a confirm on write, dirty tracking per section with Apply/Revert, a local 'previous value' revert (settings are not in the host undo history: say so in the UI), read-only/viewer disabled, host errors surfaced without losing the user's edits, a section registry API with a documented extension point for Network and Extensions sections, unit tests for the schema/validation/patch building and DOM tests with a fake host (apply, revert, forbidden for determinism, read-only, host warnings, restart badge); README updated. Frontend rules: requiresDocs is false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; NEVER run taskkill or kill other processes; keep changes inside the files your lane owns (other frontend items are in flight: README.md and keymap/panel registrations will collide mechanically: keep each as a small separate hunk; the repo README is LF); no engine/protocol change unless this item says so. Read the named design sections and the decisions (all ACCEPTED by the user) FIRST. Merged and usable: F1 panel registry/layout, F2 prefs store + keymap, FE3 rotate/scale gizmos (mode on ViewportController, W/E/R/Q keymap actions), FE5 inspector local transform, FE6/FE7 filters, FE8 history panel, MCP1 n2-mcp (mcp/), the protocol 1.14.0 typed client.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Project settings panel registered through F1 with a section registry (Network/Extensions slots)
- General, Physics, Rendering, Math, Determinism (admin-only handling, luaSeed warning), Layers, Input JSON and Raw JSON sections; minimal patches; restart badges; host warnings shown
- Read-only/viewer disabled; forbidden handled; unit and DOM tests; frontend CI green; README updated

## Notes
Reviewer: patches contain only changed keys, the determinism block is never sent by a non-admin UI path, host errors/warnings surfaced and edits kept, restart-badge list matches the engine docs, Raw JSON write confirm, section registry extensible, no secret, tests meaningful, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe9-project-settings` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe9-project-settings/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe9-project-settings --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe9-project-settings/dev.json: {"pr":<number>,"branch":"feat-fe-fe9-project-settings","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe9-project-settings` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
