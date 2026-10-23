# Task physx-linux-enable: Enable PhysX on Linux: download pinned static archives, link them, turn physics on in the Linux legs and the Docker image
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-physx-linux-enable.

## Issue / plan
Published 2026-10-13 (release physx-dependencies-linux-5.6.1 on EpicNicks/N2Engine): assets https://github.com/EpicNicks/N2Engine/releases/download/physx-dependencies-linux-5.6.1/physx-linux-x86_64-5.6.1.tar.gz (sha256 e8bd1f8f8d9f0b1d04d5f21ccdc509535ca5e56d23c0440b32f9aea5696208f5) and .../physx-linux-aarch64-5.6.1.tar.gz (sha256 22c1fffdb3a81afd38820c68745ef22abfbe687786aef9c032bb947ffacabb90). Archive layout: LICENSE.md and bin/linux.x86_64/release/*.a (or bin/linux.aarch64/release/*.a), extract at external/PhysX/physx. Libraries (8, static): libPhysX_static_64.a libPhysXCommon_static_64.a libPhysXFoundation_static_64.a libPhysXCooking_static_64.a libPhysXExtensions_static_64.a libPhysXPvdSDK_static_64.a libPhysXCharacterKinematic_static_64.a libPhysXVehicle2_static_64.a; link all inside -Wl,--start-group ... --end-group then -lpthread -ldl -lrt; consumers compile with -DNDEBUG and never PX_CHECKED/PX_DEBUG; the committed headers (5.6.1) are fine (only CRLF/comment differences from the tag, no ABI change) and stay. Full detail: orchestrator/ledger/physx-linux-build/notes.json (the dir name is linux.x86_64, not linux.clang as the older plan text below says; the PhysX build used Clang 20, no fallback needed). Decisions (user, 2026-10-13): static + Release-only, SHA256 pinned in CMake, assets on EpicNicks/N2Engine releases, PVD off on Linux server builds. Scope (PR 2, plan in orchestrator/proposals/physx-linux.md section 4): (1) CMakeLists.txt: replace the Linux force-OFF stub (~l.77-80) with arch detection (x86_64 / aarch64), download of the matching tarball with file(DOWNLOAD ... EXPECTED_HASH SHA256=...) using hashes committed in cmake/PhysXLinuxHashes.cmake, extract to external/PhysX/physx, PHYSX_LIBS = the static list inside a -Wl,--start-group ... --end-group plus pthread dl rt (list and order from the physx-linux-build notes), PHYSX_DLLS empty; a download or hash failure turns PhysX OFF with a clear warning like Windows (never silently use an unverified file). (2) engine/CMakeLists.txt ~l.49-53: link directories/paths for Linux, not only WIN32; config mapping must work for Release/RelWithDebInfo/Debug (all use the release libs; use $<IF:$<CONFIG:Debug>,...> style, not $<LOWER_CASE:$<CONFIG>>). (3) PVD: PhysXBackend must not open the PVD socket on Linux server builds (gate it off on non-Windows, document). (4) tests.yml: -DN2ENGINE_USE_PHYSX=ON for the `linux (Release, x86_64, Clang)` and `linux (Release, arm64, Clang)` legs with an actions/cache for external/PhysX/physx/bin keyed by runner.os, runner.arch and the pinned version; keep the required check name `linux (Release, x86_64, Clang)` and the GCC leg as is (OFF); the docker job picks it up through the Dockerfile. (5) deploy/Dockerfile: drop the PhysX OFF flag (static libs add nothing to the runtime image), update comments; systemd unit unchanged. (6) Tests: on Linux the engine health reports physics Ready, and the physics-conditional editor/play tests run (check their N2ENGINE_PHYSX_ENABLED paths); add a Linux physics smoke assertion to tests/smoke/editor_host_smoke.py (physics Ready in GetEngineHealth when the build has PhysX; the script must not require PhysX on Windows OFF legs). (7) Docs: docs/physics.html (~l.102), docs/editor.html Linux section, README PhysX mention, THIRD_PARTY_NOTICES.md (Linux assets, BSD-3 licence text shipped in the archive).

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^engine/
- ^tests/
- ^deploy/
- ^docs/
- ^README\.md$
- ^THIRD_PARTY_NOTICES\.md$
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green; the x86_64 and arm64 Linux legs run with PhysX ON and the physics tests pass
- Hashes pinned and verified by CMake; a bad hash turns PhysX OFF with a warning
- Docker image builds with PhysX ON and its host reports physics Ready
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Unverified: physics behaviour under real gameplay on Linux (CI tests only), arm64 PhysX NEON path under load.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs physx-linux-enable` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-physx-linux-enable` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/physx-linux-enable/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-physx-linux-enable --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/physx-linux-enable/dev.json: {"pr":<number>,"branch":"feat-physx-linux-enable","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-physx-linux-enable` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
