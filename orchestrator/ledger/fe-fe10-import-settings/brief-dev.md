# Task fe-fe10-import-settings: Electron UI FE10: typed import settings editor for assets (previews come later with EN1)
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe10-import-settings.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md (U1-U21 ACCEPTED): read section 6 (import settings UI and asset previews) FULLY and section 16 (assets lane: FE10 -> FE14). Previews/thumbnails need the engine EN1 minor (GetAssetPreview) which does NOT exist yet: NOT in this PR. Read the frontend assets panel code (assets-*.ts(x)), the existing import-settings handling, and in the engine repo the import settings commands and per-type option sets (assetimport/, protocol.json, docs) FIRST. SCOPE: section 6's FE10 part: typed per-asset-type import settings forms (fields, ranges, defaults taken from the engine's real option set; verify each against the engine code, cite where), validation before send, Apply (with the reimport the host performs) and Revert, multi-asset same-type editing only if the section asks for it, role/read-only gating; Node + DOM tests. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck and npm test (Node + DOM tests) must pass before you push; CI has 3 checks. Append ONE README.md section for the feature (other PRs also append: keep yours self-contained). Follow the existing panel/keymap/prefs registries (F1/F2) and lane file boundaries in the roadmap section 16. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Typed import settings forms matching the engine option sets
- Validation, Apply/Revert, gating
- Node + DOM tests pass; 3 CI checks green

## Notes
Reviewer: every field/range checked against the engine code, no preview code that calls non-existent commands, tests real.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe10-import-settings` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe10-import-settings/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe10-import-settings --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe10-import-settings/dev.json: {"pr":<number>,"branch":"feat-fe10-import-settings","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe10-import-settings` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
