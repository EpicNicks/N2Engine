# Task ci-ccache-windows: CI: compiler cache on the three Windows legs (Ninja + ccache/sccache, /Z7), cache saved only from master pushes
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: ci-ccache-windows.

## Issue / plan
Analysis and decisions: C:/Method Notes Obsidian/orchestrator/proposals/ci-speedup.md (read it fully first: measured per-leg times, the Windows recommendations). MEASURED RESULT after the Linux ccache item (#154) merged: Linux legs went from 15-18 min to 1-3 min on PR runs; the Windows legs are now the critical path: `windows (Release, PhysX ON)` ~30 min, `windows (Debug, PhysX ON)` ~17 min, `windows (Debug, PhysX OFF, FreeType ON, ufbx ON, TLS ON)` ~15-18 min (Build step is 85-93% of each). The user APPROVED doing the Windows cache (2026-10-15, 'do it'). CONSTRAINTS: workflow + docs only (.github/workflows/tests.yml, docs/testing.html), NO source changes; do NOT rename any job or step used as a check name (the 10 checks: linux x3, windows x3, docker, protocol, changes, web); the required check is `linux (Release, x86_64, Clang)`; the windows matrix job names must stay EXACTLY the same; the Windows legs run the editor-host smoke tests, TLS smoke, ctest, PhysX download/caching steps: keep all of them working with the new build layout (binary paths currently `build/bin/<Config>/...` from the Visual Studio generator: with Ninja single-config the paths change (`build/bin/N2EditorHost.exe`): find EVERY place in tests.yml and in tests/smoke/*.py and any script that hardcodes the multi-config path and handle it; you may edit tests/smoke scripts ONLY if truly required, preferably keep the output layout identical by setting CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>/ per-config subdirectory in the workflow's configure line if the CMake already supports it). DESIGN to evaluate and choose (state the choice and the reason in the PR): (a) Ninja generator + MSVC cl via `ilammy/msvc-dev-cmd`-style env setup (PREFER setting up the VS environment with the official `vcvars64.bat` via a run step, or the pinned third-party action if you justify it; pin exact versions of any action), with `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` (or sccache), `/Z7` instead of `/Zi` (debug info embedded so a compiler cache can work: use CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded (CMake >= 3.25) and policy CMP0141 NEW, check the repo's cmake_minimum_required and the CMake version on the runner), ccache/sccache installed via a pinned package (choco/winget or a pinned release download with checksum) and its cache directory restored/saved with actions/cache keyed per leg + a rotating suffix exactly like the Linux job (copy the Linux ccache pattern from the merged #154 in tests.yml: restore on PR, SAVE ONLY on push to master (event_name == push and ref master), `ccache -s`/sccache stats in the job summary, `continue-on-error` on cache steps so a cache failure never fails the build); (b) keep the VS generator and only cache downloads (rejected unless (a) is impossible). PhysX on Windows is prebuilt binaries: check how the build links them and that the Ninja build still finds them. Also consider Debug vs Release config differences (per-leg caches must not share), and that MSVC Debug uses /RTC1 and /MDd: fine for ccache. Since nothing can be built locally, THE PR's OWN CI RUN is the test; expect the first run to be cold; the saving is measured on the next PR after merge. Because a bad Windows layout change could break the three Windows legs, write defensively, re-read YAML, and list every unverified item: whether the Ninja+cl build produces byte-for-byte equivalent tests behaviour (the tests must pass), smoke test paths, PhysX DLL copy steps, whether ccache supports MSVC /Z7 PDB-less objects, cache key behaviour. Docs: update docs/testing.html CI section (what is cached on Windows, the generator change, how to read the stats, only master saves).

## Allowed paths (regex, repo-relative)
- ^\.github/workflows/
- ^docs/testing\.html$
- ^tests/smoke/
- ^cmake/
- ^CMakeLists\.txt$

## Acceptance
- All 10 checks keep their exact names and are green on the PR; the three Windows legs build through Ninja + a compiler cache with per-leg caches
- Caches are saved only from master push runs and only read on PRs; cache statistics in the job summary
- A cache restore/save failure never fails a job
- Smoke tests, TLS smoke and ctest still run with correct paths; PhysX legs still link
- docs/testing.html documents the Windows cache and the generator change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: step through the Windows jobs as GitHub would: generator and compiler environment, every path that used the multi-config layout (grep build/bin and Release/Debug in tests.yml, tests/smoke, cmake), cache key/restore-keys logic and that PRs cannot save, job names unchanged, /Z7 policy, continue-on-error placement, nothing else in the jobs changed. Unverified: warm-run saving (measured after the first master run), that all three legs pass with Ninja.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs ci-ccache-windows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref ci-ccache-windows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/ci-ccache-windows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head ci-ccache-windows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/ci-ccache-windows/dev.json: {"pr":<number>,"branch":"ci-ccache-windows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/ci-ccache-windows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
