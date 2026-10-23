# Proposal: PhysX on Linux (x86_64 + aarch64) and physics on the Linux editor host

Status: research only. Marks: [V] verified in the repo at master 58c67d2; [M] from memory of NVIDIA's repo, UNVERIFIED (no network/build here, must be proven by the first CI run).

## 1. How Windows PhysX works today [V]
- Headers are COMMITTED: `external/PhysX/physx/include/**` (95 entries, incl. gpu/, cudamanager/, pvd/, omnipvd/). Version = **PhysX 5.6.1** (`external/PhysX/physx/include/foundation/PxPhysicsVersion.h:48-50`). Only headers are in git (`git ls-files external/PhysX`); no source.
- Binaries: `CMakeLists.txt:34-37` downloads `https://github.com/EpicNicks/N2Engine/releases/download/physx-dependencies/bin.zip` (one release on THIS repo, tag `physx-dependencies`, asset `bin.zip`) when `external/PhysX/physx/bin/win.x86_64.vc143.mt/debug/PhysX_64.dll` is missing (l.55-69), extracts to `external/PhysX/physx` (so zip root is `bin/win.x86_64.vc143.mt/{debug,release}/`). **No hash check, no version in the asset name.** Failure only warns and turns PhysX OFF (l.61-62).
- Windows libs are DLL-based: `PhysX_64, PhysXCommon_64, PhysXFoundation_64, PhysXCooking_64` (shared) + `PhysXExtensions_static_64, PhysXPvdSDK_static_64` (l.40-53); optional GPU dll l.72-75. DLLs are copied next to test_project (`test_project/CMakeLists.txt:28-42`) and lua_project (`lua_project/CMakeLists.txt:26-29`).
- Engine wiring: `engine/CMakeLists.txt:45-52`: `N2ENGINE_PHYSX_ENABLED=1` (PUBLIC), include dir, link `${PHYSX_LIBS}`, `target_link_directories` only inside `if (WIN32)`. `test_project` additionally links libs (`CMakeLists.txt:115-119`).
- Linux branch is a force-OFF with warning: `CMakeLists.txt:77-80`. Linux CI/Docker pass `-DN2ENGINE_USE_PHYSX=OFF`: `tests.yml:171, 217, 264`, `deploy/Dockerfile:33`. Comments to update: `tests.yml:150`, `Dockerfile:11`, `docs/physics.html:102`.
- CI cache: `tests.yml:87-93` `actions/cache@v6`, path `external/PhysX/physx/bin`, key `physx-bin-${{ runner.os }}-v1` (not per config; the zip holds debug+release). Windows only.
- Licence: `THIRD_PARTY_NOTICES.md:34` already lists "NVIDIA PhysX (binaries) ... downloaded from `physx-dependencies` release ... BSD 3-Clause". Needs text widening, not a new entry.
- Not in repo: how bin.zip was originally built (commit 4e2b6fa "added PhysX build"). Ask the user whether they still have the build recipe/PhysX checkout.

## 2. Building PhysX 5.6.1 on Linux (mostly [M])
- Source: github.com/NVIDIA-Omniverse/PhysX, tag `107.3-physx-5.6.1`-style tags exist on that repo (exact tag name [M], verify with `git ls-remote --tags`); BSD-3 for the whole 5.x tree. Use the same tag the committed headers come from; diff generated headers after the first build to confirm.
- Build entry: `physx/generate_projects.sh <preset>` then `cmake --build physx/compiler/<preset>-release` (preset dirs like `linux-release`) [M]. Presets in `physx/buildtools/presets/`: `linux` (x86_64, clang), `linux-aarch64` (clang, cross-compile oriented) [M]. The aarch64 preset may expect a cross toolchain file; on a native arm runner we may need a custom preset XML or direct `cmake -S physx/compiler/public -DTARGET_BUILD_PLATFORM=linux -DPX_OUTPUT_ARCH=arm ...` [M, biggest unknown].
- Toolchain: PhysX's scripts use **packman** (downloads cmake, python, clang from NVIDIA CDN, needs network at build time) [M]. Prefer bypassing: call cmake directly with `CC=clang-20`/system cmake/python3. Docs say clang 9-ish through ~14 tested; Clang 20 will likely hit new warnings promoted by PhysX's own `-Werror` [M]. Mitigation: patch `physx/source/compiler/cmake/linux/CMakeLists.txt` flags in CI (`-Wno-error`, or add `-Wno-error=...` list) and pass `--gcc-install-dir=/usr/lib/gcc/<arch>-linux-gnu/14` so it matches the engine's libstdc++ 14 (consistent C++ ABI; PhysX's public API is mostly pure virtual/C-like, but `PxVec3` etc. are templated headers compiled by the engine, so keep the same stdlib).
- Known required switches [M]: disable GPU/CUDA (`PX_BUILDPVDRUNTIME` fine; set `PX_GENERATE_GPU_PROJECTS=False`, `PX_GENERATE_GPU_PROJECTS_ONLY=False`, no PhysXGpu), `PX_BUILDSNIPPETS=False`, `PX_BUILDPUBLICSAMPLES=False`, `PX_GENERATE_STATIC_LIBRARIES=True` (Linux preset default is static `.a`, names `libPhysX_static_64.a`, `libPhysXCommon_static_64.a`, `libPhysXFoundation_static_64.a`, `libPhysXCooking_static_64.a`, `libPhysXExtensions_static_64.a`, `libPhysXPvdSDK_static_64.a`, `libPhysXCharacterKinematic_static_64.a`, `libPhysXVehicle*`, `libSceneQuery_static_64.a`, `libSimulationController_static_64.a`, `libLowLevel*_static_64.a`, `libLowLevelAABB`, `libLowLevelDynamics`, `libFastXml`) [M]. Static means the engine link line must list ALL of them in dependency order wrapped in `-Wl,--start-group ... --end-group`, plus `pthread dl rt`. Static avoids the Windows DLL-copy and the `LD_LIBRARY_PATH`/rpath problem in the systemd unit and Docker runtime image (only OpenAL is shared there). Needs `-fPIC`? Only if linked into a .so; host is an executable, so not required.
- Configs: build `release` for CI/Docker; also `debug` only if the Linux Debug leg exists (it does not; Linux CI is Release only). `checked`/`profile` not needed. Size/time: [M] ~15-30 min per arch on 4 cores for release static, ~1-1.5 GB tree.
- Arm: PhysX uses NEON intrinsics (`PX_SIMD_DISABLED` unset on arm) [M]; the engine math library being scalar-only on aarch64 (`math/CMakeLists.txt:35-42`, `math/include/math/CpuInfo.hpp:3-12`) is independent: we convert at the boundary (`PxVec3(x,y,z)`), no shared SIMD types.

## 3. Engine side
- `engine/src/physics/physx/PhysXBackend.cpp` (2178 lines) uses `PxCreateFoundation` (l.166), `PxCreatePhysics` (l.190), `PxDefaultCpuDispatcherCreate(4)` (l.205), PVD socket on 127.0.0.1:5425 under `__has_include(<pvd/PxPvd.h>)` (`PhysXBackend.hpp:24-28`; headers are committed so it will be 1 on Linux and needs `PhysXPvdSDK` + `PxDefaultPvdSocketTransportCreate` linked; it will just fail to connect when no PVD runs, harmless; consider gating off on Linux server builds). Includes are `<PxPhysicsAPI.h>` etc. against `external/PhysX/physx/include`: identical on Linux.
- No `_WIN32`/MSVC-specific code in the backend [V grep for DLL/_WIN32/_MSC returned none]; 10 uses of `N2ENGINE_PHYSX_ENABLED` elsewhere, all feature gates.
- Needed CMake change: replace the `elseif(UNIX AND NOT APPLE)` stub (l.77-80) with: arch detect (`CMAKE_SYSTEM_PROCESSOR` x86_64 -> `linux.clang`, aarch64 -> `linux.aarch64` [dir names M]), same download/extract, `PHYSX_LIB_DIR=.../bin/<dir>/release` (Release only; map Debug/others to release libs), `PHYSX_LIBS` = static list inside a linker group, `PHYSX_DLLS` empty. In `engine/CMakeLists.txt:49-53` the `target_link_directories` is WIN32-only: extend to Linux (or use full paths). Note `$<LOWER_CASE:$<CONFIG>>` is invalid for multi-word configs like RelWithDebInfo; use `$<IF:$<CONFIG:Debug>,debug,release>`.
- `PX_PHYSX_STATIC_LIB` must be defined when linking static PhysX on Windows only; on Linux not needed [M]. Do NOT define `NDEBUG`/`_DEBUG` mismatch: PhysX release libs expect `NDEBUG`; CMake Release provides it. Mixing a checked/debug header define (`PX_CHECKED`, `PX_DEBUG`) with release libs breaks ABI, so never define them.
- Alignment/ABI: PhysX types (`PxVec3` 12B, `PxTransform` 32B with 16B-aligned quat in some builds) never cross into engine data layouts [needs spot check: backend keeps `PxRigidActor*` in maps]. Math library SSE types (`__m128`, `alignas(16) SimdStorage`) are not passed to PhysX. Risk is low; on arm the scalar `SimdStorage` is 16B aligned too (`CpuInfo.hpp:28-30`). The Clang+libstdc++ 14 combination is not a PhysX ABI issue (no std types in the PhysX API).
- Tests: add a Linux physics smoke (health reports Physics "Ready" not Disabled; reuse `tests/engine/HealthTests.cpp` / `ApplicationTickTests.cpp` which mention PhysX; `tests/editor-server/project/EditorPlayTests.cpp`, `EditorPickingTests.cpp`, `EditorViewportTests.cpp` already have PhysX-conditional paths: check their `N2ENGINE_PHYSX_ENABLED` skips run on Linux).

## 4. Plan
### PR 1: workflow `physx-linux.yml` (no engine change)
- Triggers: `workflow_dispatch` (inputs: physx_tag, release_tag) and `push` of tag `physx-linux-*`. Matrix `{ubuntu-24.04: x86_64, ubuntu-24.04-arm: aarch64}` (arm runner free for public repos; confirm repo is public).
- Steps: install clang-20 + libstdc++-14-dev + cmake + ninja + python3 (same apt block as `tests.yml:160-166`); `git clone --depth 1 --branch <tag> NVIDIA-Omniverse/PhysX`; patch -Werror; cmake release static, GPU off, snippets off; `cmake --build --parallel`; collect `physx/bin/<dir>/release/*.a` into `physx-linux-<arch>-<ver>.tar.gz` (`bin/<dir>/release/*.a` layout mirrors the Windows zip so extraction is `${PHYSX_ROOT}/physx`); write `.sha256`; verify `nm`/`file` shows right arch; upload via `gh release upload` to a release (needs `contents: write`). Also `actions/upload-artifact` for dry runs. Cache by PhysX tag + patch hash.
- Hosting decision: **recommend a new release tag on EpicNicks/N2Engine (e.g. `physx-dependencies-linux-5.6.1`)**, not a separate repo: same place as the Windows zip (`CMakeLists.txt:37`), one repo to audit, `GITHUB_TOKEN` suffices, no new credentials. Keep the old `physx-dependencies` tag untouched (Windows CI depends on it). A separate repo only if the user wants binaries out of the main repo's release list.
- Engine download (in PR 2): asset name carries version+arch, CMake `file(DOWNLOAD ... EXPECTED_HASH SHA256=<pinned>)` with hashes committed in a small `cmake/PhysXLinuxHashes.cmake` (per arch). Optionally retrofit the Windows zip with a hash later (separate PR; changes nothing for users but would break if someone re-uploads bin.zip).
- Verification of PR 1 itself: run via workflow_dispatch on the PR branch; download artifact; link a 20-line `PxCreateFoundation` test on both runners (add as a final workflow step) before publishing.

### PR 2: enable on Linux
- `CMakeLists.txt:77-80` rewrite as above; `engine/CMakeLists.txt:49-53` link dirs/group; update warning text; macOS stays silently absent.
- `tests.yml`: flip `-DN2ENGINE_USE_PHYSX=ON` on linux x86_64 Clang (l.171) and arm64 (l.217) legs; add `actions/cache` (key `physx-bin-${{ runner.os }}-${{ runner.arch }}-v2`, path `external/PhysX/physx/bin`) like l.87-93; `linux-gcc` (l.247, informational, GCC 14, l.264) can stay OFF (PhysX static libs built by Clang link fine with GCC but keep the informational job unchanged); the `docker` job (l.277) builds the Dockerfile and picks PhysX up from it. The required check name `linux (Release, x86_64, Clang)` must not change. Update l.150 comment.
- `deploy/Dockerfile`: drop `-DN2ENGINE_USE_PHYSX=OFF`, add `ca-certificates curl` (already present) so CMake download works in build; static PhysX adds nothing to runtime image. Network access during `docker build` is required (already needed for apt). Update l.11 comment.
- `deploy/n2editorhost.service`: no change (static). Docs: `docs/physics.html:102`, `docs/editor.html` Linux section, README PhysX mention, `THIRD_PARTY_NOTICES.md:34` (add Linux assets + PhysX BSD-3 text/copyright preserved in the tarball: ship `LICENSE.md` of PhysX inside each archive, binary redistribution needs it).
- Tests as in section 3.

## Risks / unknowns
1. Linux preset names, output dir names, library file list, and packman dependence are from memory: PR 1 exists to discover them; budget 2-4 iteration runs.
2. Clang 20 vs PhysX warnings/`-Werror`; may need patching sources (e.g. `-Wno-error=...`) or using clang-18 for the PhysX build only (static lib + C++ API, so mixing compilers is acceptable if both use libstdc++).
3. aarch64 preset may assume cross-compiling; native runner needs preset tweaks.
4. Link-order/missing-symbol churn with static libs (use link group).
5. Hash pinning means each rebuild needs a PR to bump hashes; CI `Cache PhysX` key must include version.
6. Build time 15-45 min per arch per run (only on dispatch/tag, not per PR); asset size ~100-300 MB/arch [M].
7. PhysX 5.6.1 Windows header set vs the Linux source tag must be identical; diff generated headers.
8. PhysX PVD socket on a server host: keep or disable.
9. Windows bin.zip has no hash (pre-existing).

## Decisions for the user
- Host assets on EpicNicks/N2Engine (recommended) vs a new repo.
- Static libs (recommended) vs shared `.so`.
- Pin by hash in CMake (recommended) vs trust the release URL (as Windows).
- Release-only Linux binaries (recommended) vs also debug.
- Does a checkout/recipe of the original Windows PhysX build exist (to reuse the same tag/patches)?
- OK to compile PhysX with Clang 20, or fall back to Clang 18 for PhysX only if warnings prove intractable?
- Keep PVD enabled on Linux?
