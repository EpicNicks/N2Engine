# Task fe-fe9b-layers-matrix: Electron UI FE9b: layer collision matrix editor (32x32 checkbox grid) in the project settings Layers section
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe9b-layers-matrix.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md (U1-U21 ACCEPTED): section 5 (project settings; FE9b row: the layers collision matrix as a 32x32 checkbox grid in a second PR) and section 16 (panels lane: FE9 -> FE9b -> FE15). MERGED: FE9 project settings panel (frontend #43: section registry, typed tables in project-settings-schema.ts, Layers section with 32 names and a READ-ONLY note about the collision matrix). Read the FE9 code, its README section, and in the engine repo the project settings format for layers and the collision matrix (engine physics layer matrix and how SetProjectSettings validates it: find it in the engine code and docs/physics.html / docs/project-settings docs) FIRST: the frontend must send exactly the shape the host validates. SCOPE: replace the read-only note with an editable symmetric matrix (toggling (a,b) toggles (b,a)), rows/columns labelled with layer names (unnamed layers collapsed by default with a show-all toggle), row/column select-all helpers, dirty tracking and Apply/Revert consistent with the other FE9 sections, admin/role gating the same as other settings writes, read-only when the host is read-only or a viewer; DOM tests (toggle symmetry, apply sends the right payload, revert, role gating, 32-layer bounds) and unit tests for the matrix model. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test (Node + DOM tests) must pass before you push; CI has 3 checks. Append ONE README.md section for the feature (other PRs also append: keep yours self-contained). Follow the existing panel/keymap/prefs registries (F1/F2) and lane file boundaries in the roadmap section 16. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Editable symmetric 32x32 matrix in the Layers section sending exactly what the host validates
- Apply/Revert/dirty and role gating like the other sections
- Node + DOM tests pass; 3 CI checks green

## Notes
Reviewer: payload matches the engine validation (read the engine code), symmetry, bit order, gating, tests real.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe9b-layers-matrix` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe9b-layers-matrix/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe9b-layers-matrix --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe9b-layers-matrix/dev.json: {"pr":<number>,"branch":"feat-fe9b-layers-matrix","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe9b-layers-matrix` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
