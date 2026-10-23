# Task docs-particles-gpu-verified: Docs: particles page records the manual GPU verification and drops the stale 'Vulkan is stubs' and 'not confirmed by a run' claims
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: docs-particles-gpu-verified.

## Issue / plan
Docs-only fix. docs/particles.html (the 'Not verified in CI' callout around lines 294-298 on master) is stale in two ways: (1) it says the PhysX driver's assumptions are 'from memory of PhysX 5.6.1 and not confirmed by a run' and that the GPU test and checklist 'are what check them': the maintainer HAS now run the GPU golden test and the manual checklist (docs/gpu-particles-checklist.html) on a Windows PC with an NVIDIA GPU on 2026-10-14 and reported that it WORKED. Rewrite that paragraph to say exactly that and no more: the real PhysX driver is still never run in CI (it needs a CUDA context), but it was verified manually by the maintainer on a Windows GPU PC on 2026-10-14 (GPU golden test + checklist passed); the points listed (device pointers updated by host-to-device copy plus raiseFlags, buffer order, setNbActiveParticles/flags between steps, release order) are what that run exercised; be HONEST about what was NOT recorded: no VRAM figures, timings, or tuning of the water/sand/smoke presets were captured (the presets remain untuned), and nothing was verified on other GPUs/drivers or on Linux (the Linux GPU asset is deferred). Keep the list of what remains unverified (other vendors/drivers, Linux GPU, performance) and keep the link to the checklist. (2) it says 'The Vulkan renderer, which is still stubs': on master the Vulkan renderer now has a headless core (stage 0), resource creation (stage A) and, once PR #144 merges, unlit draws (stage B) but still has NO blending, text or swapchain, so particles (additive/alpha-blended billboards) do not draw on Vulkan yet: reword that sentence accordingly WITHOUT claiming more than master has (check docs/application.html and git log origin/master for the current Vulkan status wording and stay consistent with it; do not mention PR #144 if it is not merged yet when you start; if it IS merged, say unlit meshes draw but blending/text do not). Also grep docs/physics.html, docs/soft-bodies.html, docs/gpu-particles-checklist.html, docs/index.html and docs/testing.html for the same two stale claims and fix them the same way (minimal edits; keep the existing HTML style, badges and CRLF/LF per file: check the file's line endings before editing and keep the diff small). Do not change any other page content. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`). Check the final diff is only the intended sentences.

## Allowed paths (regex, repo-relative)
- ^docs/

## Acceptance
- All Tests-workflow jobs green
- particles.html records the manual GPU verification honestly (what was and was not recorded)
- No page claims the Vulkan renderer is still all stubs
- Diff limited to the stale sentences

## Notes
Reviewer: make sure no claim goes beyond what the user reported ('the GPU test and checklist worked on the Windows GPU PC', no numbers recorded) and that Vulkan wording matches master. Docs only.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs docs-particles-gpu-verified` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref docs-particles-gpu-verified` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/docs-particles-gpu-verified/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head docs-particles-gpu-verified --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/docs-particles-gpu-verified/dev.json: {"pr":<number>,"branch":"docs-particles-gpu-verified","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/docs-particles-gpu-verified` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
