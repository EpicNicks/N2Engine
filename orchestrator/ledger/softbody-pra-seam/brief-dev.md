# Task softbody-pra-seam: Soft bodies PR-A: ISoftBodyBackend seam, SoftBodies health row and per-feature GPU failure state
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-softbody-seam.

## Issue / plan
Approved by the user 2026-10-14: proposal C:/Method Notes Obsidian/orchestrator/proposals/physx-soft-bodies.md (read it FULLY first, sections 1, 2 ('GPU status coupling (must fix in PR-A)'), 5 PR-A, 6, and 'Decisions (user, 2026-10-14)'). This item is stage PR-A ONLY (CI-verifiable, no hardware): do not implement the component, tetrahedralisation, CPU solver or any PhysX deformable-volume calls (those are PR-B and PR-C). Particles PR-C (#129) is MERGED into master: read its design (engine/include/engine/physics/IParticleBackend.hpp, the generation-counted handle helper, PbdDriver/PhysXPbdDriver, PhysXBackend.cpp GPU probe and FailParticles/PollParticleFailure, GpuStatus.hpp, Application.cpp health rows, tests/engine/physics/GpuProbeTests.cpp and the fake driver pattern) first and mirror its style. Scope: (1) a new interface engine/include/engine/physics/ISoftBodyBackend.hpp (name per the proposal) with the minimal seam the proposal specifies (create/destroy system by handle, step/update, read-back of vertex positions, name/status), reusing the SHARED generation-counted handle helper: if the particles code embeds the generation counter in IParticleBackend, factor it into a small shared header/class used by both with NO behaviour change for particles (particles tests must pass unchanged); `IPhysicsBackend::GetSoftBodyBackend()` returning nullptr by default (and for PhysXBackend until PR-C). (2) Split the GPU failure state per feature: today PR #129's `FailParticles` rewrites the single `_gpuStatus`; change it so the scene/probe status stays in `_gpuStatus` and each feature (particles, softBodies) has its OWN failure state; each health row composes scene status + its own failure, and `Physics.IsGpuAvailable` reflects the scene/probe only; a soft-body failure must never flip the Particles row and vice versa; existing behaviour of the Particles row and the existing particle failure-chain tests must remain identical (extend, do not weaken, those tests). (3) Add the `SoftBodies` health row beside Particles at Application.cpp: Disabled everywhere in CI with the detail wording the proposal gives (generic SubsystemStatus: no protocol change), and document the row. (4) Tests (all CI-verifiable, run on every leg): the shared handle helper (generation invalidates old handles even at the same address), the default null seam, the SoftBodies row is Disabled with the right detail on a CPU-only build and with physics.gpu off/auto/on when no GPU, the failure-state split (inject a particle failure through the existing InstallParticleDriverForTesting seam: the SoftBodies row is unaffected; and a fake soft-body failure hook, if you add one for the test, does not flip Particles), Lua `Physics.IsGpuAvailable` unchanged. (5) Docs: docs/physics.html (health rows, per-feature failure semantics) and docs/testing.html rows; keep the 'SoftBodies' wording consistent with the proposal. No protocol change. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`; Windows PhysX-ON legs compile PhysXBackend).

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^cmake/

## Acceptance
- All Tests-workflow jobs green
- ISoftBodyBackend seam + shared generation-counted handle helper, particles behaviour unchanged (existing particle tests pass unchanged)
- Per-feature GPU failure state: a particle failure never flips the SoftBodies row and the reverse; Physics.IsGpuAvailable reflects the scene probe only
- SoftBodies health row Disabled in CI; docs updated; no protocol change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: confirm the refactor of the particle failure coupling does not change particle behaviour (health row text, Physics.IsGpuAvailable, fallback to CPU), the shared handle helper keeps the generation semantics, no PhysX deformable calls leaked in, tests prove the split in both directions.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs softbody-pra-seam` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-softbody-seam` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/softbody-pra-seam/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-softbody-seam --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/softbody-pra-seam/dev.json: {"pr":<number>,"branch":"feat-softbody-seam","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-softbody-seam` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
