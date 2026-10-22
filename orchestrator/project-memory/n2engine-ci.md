---
name: n2engine-ci
description: "N2Engine can't be built on this machine; verify changes via the GitHub Actions workflow on a PR"
metadata:
  node_type: memory
  type: project
  originSessionId: 4372d5e7-f24f-4cce-984c-9dbed3438b38
  modified: 2026-09-29T17:26:04.022Z
---

The user doesn't want to install the N2Engine toolchain (CMake, MSVC, Vulkan SDK) on this machine, so nothing can be built or tested locally.

**How to apply:** push changes to a branch, open a PR (as EpicNicks, see [[github-accounts]]), and verify with the `Tests` workflow (`.github/workflows/tests.yml`, windows-latest, added in PR #7 on 2026-09-29). Watch with `gh run watch <id> --repo EpicNicks/N2Engine` in the background; a full build takes ~7–10 minutes. ctest only prints gtest detail on failure.

**Why:** a complex set of requirements the user explicitly said they won't install.

**Update 2026-10-13:** the `Tests` workflow now has 8 checks: protocol; windows x3 (Debug PhysX ON, Release PhysX ON, Debug PhysX OFF/FreeType/ufbx/TLS ON); `linux (Release, x86_64, Clang)` (the REQUIRED check name the user must set in branch protection; Clang 20 from apt.llvm.org on libstdc++ 14, PhysX ON, runs ctest and the Python smoke steps); `linux (Release, arm64, Clang)` and `linux (Release, x86_64, GCC 14)` (both NOT required, both green); `docker (editor host image, x86_64)` (not required). Other workflows: `PhysX Linux` (physx-linux.yml) and `PhysX Windows slim` (physx-windows-slim.yml), dry runs on PR, publish only by dispatch/tag. Quick server-side branch update: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api -X PUT repos/EpicNicks/N2Engine/pulls/<n>/update-branch` (no agent needed); ci.json from ci-wait.mjs can be stale, so check the PR head's check-runs via the wrapper. See [[orchestrator-lessons]].
