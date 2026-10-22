# Task fe-fe6-fe7-filters: Electron UI FE6 + FE7: hierarchy search/filter and console filters (level, source/tag, text) with persistence and collapse of repeats
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe6-fe7-filters.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md section 8 (Candidate 5: hierarchy search/filter and console filters; FE6 hierarchy, FE7 console), decisions U20 (client-side source filter now, engine log categories later). FE7 must land before E-X4 (which adds an Extension console source: leave a clean extension point for extra sources). Read src/renderer/hierarchy-panel.tsx, hierarchy-state.ts, hierarchy-tree.ts, console-panel.tsx, console-store.ts and the README sections first. SCOPE: FE6: a search box in the hierarchy panel (name substring, case-insensitive, optional 'component:Type' and 'tag:' style tokens ONLY if the roadmap lists them: implement exactly the roadmap's list), filtering keeps ancestors of matches visible and expanded, shows match counts, clear button and Escape, selection and keyboard navigation work on the filtered view, drag-and-drop reparenting is disabled or safe while filtered (follow the roadmap), the filter text persists per project through the F2 prefs store (debounced), large scenes stay responsive (O(n) filtering, memoised); FE7: console filters: level toggles (existing), a SOURCE filter (client-side: derive the source/tag from the log line prefix convention the engine uses: look at how lines are produced (Logger categories/prefixes) and #191's public/admin tagging; unknown sources group as 'engine'), free-text filter with highlight, regex off by default, a 'collapse repeated lines' toggle with a count badge, 'clear', filters persisted per project via prefs, an extension point (a source registry) so E-X4 can register 'Extension'; counts per level stay correct under filters; performance with 10k lines (virtualised or capped as the store already does: do not regress), all pure logic in tested files; DOM tests for both panels; README updated. Frontend rules: requiresDocs false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; keep changes inside the files your lane owns (other frontend lanes are in flight at the same time: keymap action registrations and README additions will conflict mechanically, keep them as small separate hunks); no engine/protocol change unless this item says so. Read the roadmap section named in this item, decisions U1-U21 (all ACCEPTED by the user on 2026-10-20), the frontend README and the code you touch FIRST; F1 (panel registry/layout) and F2 (prefs store and keymap) are merged: use them (register panels through the registry, shortcuts through the keymap, preferences through the prefs store).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Hierarchy search with ancestor-preserving filter, counts, keyboard-safe, persisted per project
- Console level/source/text filters, repeat collapse and persistence; a source registry extension point
- Responsive on large inputs; unit and DOM tests; frontend CI green; README updated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: filtering keeps selection/keyboard navigation correct, reparent drag safe while filtered, persistence guarded and per project, source derivation robust to unknown prefixes, repeat collapse counts right, no performance regression (10k lines), tests meaningful, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe6-fe7-filters` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe6-fe7-filters/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe6-fe7-filters --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe6-fe7-filters/dev.json: {"pr":<number>,"branch":"feat-fe-fe6-fe7-filters","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe6-fe7-filters` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
