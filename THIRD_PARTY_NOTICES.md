# Third-party notices

N2Engine itself is MIT-licensed (see `LICENSE.md`). This file lists the third-party code and data the
engine builds with, where each comes from and its licence, and holds the notices that some licences
require in a product's documentation. Each component's full licence text is in its own files or in its headers
(vendored code: the header or licence file under `external/`; fetched code: its source, downloaded at
configure time into the build tree). This file is a record of what the repository contains, not legal advice.

## Vendored (in this repository)

| Component | Path | Licence |
|---|---|---|
| stb_truetype 1.26 | `external/stb_truetype/` | MIT or public domain (Unlicense), at your choice |
| stb_vorbis 1.22 | `external/stb_vorbis/` | MIT or public domain (Unlicense), at your choice |
| stb_image 2.30 (nothings/stb commit `2c980bb`), with stb_image_write 1.16 for the tests only | `external/stb_image/` | MIT or public domain (Unlicense), at your choice |
| cgltf 1.15 (jkuhlmann/cgltf tag `v1.15`, which bundles jsmn) | `external/cgltf/` | MIT, Copyright (c) 2018-2021 Johannes Kuhlmann (`external/cgltf/LICENSE`; jsmn is MIT too) |
| ufbx 0.23.1 (ufbx/ufbx tag `v0.23.1`, single-file `ufbx.c` and `ufbx.h`; optional FBX and OBJ import, compiled only with `N2ENGINE_MODEL_UFBX`) | `external/ufbx/` | MIT or public domain (Unlicense), at your choice; Copyright (c) 2020 Samuli Raivio (`external/ufbx/LICENSE`) |
| Mbed TLS 3.6.7 (Mbed-TLS/mbedtls tag `mbedtls-3.6.7`, the 3.6 long-term-support line, unmodified: `library/` and `include/` only; optional TLS for the editor host, compiled only with `N2ENGINE_EDITOR_TLS`) | `external/mbedtls/` | Apache-2.0 OR GPL-2.0-or-later, at your choice; this project uses it under Apache-2.0 (`external/mbedtls/LICENSE`); source files carry "Copyright The Mbed TLS Contributors" |
| MikkTSpace (mmikk/MikkTSpace commit `3e895b4`, 2020-03-25; tangent generation for normal maps) | `external/mikktspace/` | zlib, Copyright (C) 2011 by Morten S. Mikkelsen (`external/mikktspace/LICENSE`, and the notice at the top of both files) |
| dr_libs (dr_wav 0.14.3, dr_flac 0.13.2, dr_mp3 0.7.3, as stated in the headers; the dr_mp3 header dates 0.7.3 "TBD", so it may be a pre-release snapshot) | `external/dr_libs/` | Public domain (Unlicense) or MIT-0, at your choice |
| Lua 5.4.8 with local patches (Copyright (C) 1994-2025 Lua.org, PUC-Rio; two N2Engine patches, each marked "N2Engine local patch" in the file, both calling a switch in the added files `external/lua/n2seed.h` and `external/lua/n2seed.c` (not part of Lua): `luai_makeseed` in `lstate.c`, and `randseed` in `lmathlib.c` (the initial `math.random` state); when a seed is configured (`determinism.luaSeed`) the string hash seed and the `math.random` state are a function of that seed alone, and when none is set (the default) both behave exactly as upstream Lua) | `external/lua/` | MIT (text at the end of `external/lua/lua.h`) |
| VulkanMemoryAllocator 3.4.0 (GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator tag `v3.4.0`, commit `3aa9212`, unmodified: the single header `vk_mem_alloc.h` and `LICENSE.txt`; GPU memory for the Vulkan renderer, compiled only with `N2ENGINE_RENDERER_VULKAN`) | `external/VulkanMemoryAllocator/` | MIT, Copyright (c) 2017-2026 Advanced Micro Devices, Inc. (`external/VulkanMemoryAllocator/LICENSE.txt`) |
| sol2 3.2.3 with local patches (Copyright (c) 2013-2020 Rapptz, ThePhD and contributors; two N2Engine patches to `sol.hpp`, each marked "N2Engine local patch" in the file: the `optional<T&>::emplace` body, because upstream calls a `construct()` that does not exist, a hard error under Clang, and a unique type name per lambda under GCC, because upstream gave every same-signature lambda one name and so one shared Lua metatable) | `external/sol2/` | MIT (text at the top of `sol.hpp`) |
| nlohmann/json 3.12.0 (Copyright (c) 2013-2025 Niels Lohmann) | `external/nlohmann/` | MIT (SPDX tag at the top of `json.hpp`) |
| TinySHA1 (Copyright (c) 2012-22 Saurav Mohapatra; no version is recorded in the header) | `external/TinySHA1/` | ISC-style permissive licence (in the header) |
| glad 0.1.36 (generated OpenGL loader, `gl` specification, generated 2025-09-14 per the banner of `glad.c`) | `external/glad/` | Generated code with no licence text of its own; `external/glad/include/KHR/khrplatform.h` carries the Khronos notice (Copyright (c) 2008-2018 The Khronos Group Inc., MIT-style) |
| Noto Sans Regular 2.015 (subset; the default font; embedded in every binary) | `engine/assets/fonts/` | SIL Open Font License 1.1, Copyright 2022 The Noto Project Authors (`engine/assets/fonts/OFL.txt`; this file is the OFL text and copyright notice for the embedded font) |
| PhysX headers (the headers only; the libraries are in the fetched table below) | `external/PhysX/physx/include/` | BSD 3-Clause, Copyright (c) 2008-2025 NVIDIA Corporation, Copyright (c) 2004-2008 AGEIA Technologies, Inc., Copyright (c) 2001-2004 NovodeX AG (all three lines in each header) |
| Test and demo assets (textures, `ui/icon.png`, `smoke_robot.gltf`, the test models) | `lua_project/assets/`, `tests/assets/` | This repository's own work, generated by `lua_project/tools/make_smoke_assets.py` and `tests/assets/models/make_sample_models.py`; under the repository licence (MIT, see `LICENSE.md`) |

## Fetched or downloaded at configure time

| Component | Where it comes from | Licence |
|---|---|---|
| GLFW 3.4 | `FetchContent` in `renderer/CMakeLists.txt` | zlib/libpng |
| OpenAL Soft 1.24.0 (kcat/openal-soft tag `1.24.0`, unmodified; source: <https://github.com/kcat/openal-soft/tree/1.24.0>) | `FetchContent` in the root `CMakeLists.txt` | GNU LGPL (see `COPYING` in its source tree); see "OpenAL Soft (LGPL)" below |
| NVIDIA PhysX 5.6.1 (libraries and DLLs; Copyright (c) 2008-2025 NVIDIA Corporation) | Downloaded from this repository's `physx-dependencies-windows-5.6.1` release (Windows: the slim `physx-windows-x86_64-5.6.1-slim.zip`, derived from the original `bin.zip` whose build recipe is unknown, SHA256 pinned in `cmake/PhysXWindowsHashes.cmake`) or `physx-dependencies-linux-5.6.1` release (Linux: static libraries built from the PhysX 107.3-omni-and-physx-5.6.1 tag, SHA256 pinned in `cmake/PhysXLinuxHashes.cmake`; each archive carries PhysX's `LICENSE.md`) into `external/PhysX/` when `N2ENGINE_USE_PHYSX` is ON (the headers are vendored in `external/PhysX/physx/include/`, see above), or, for the web (Emscripten) build, compiled from source by `scripts/web-build-physx.sh` (see "PhysX on the web" below). Any third-party notices shipped inside the native PhysX archives have not been verified | BSD 3-Clause |
| FreeType 2.13.3 (optional) | `FetchContent` in `text/CMakeLists.txt` when `N2ENGINE_TEXT_FREETYPE` is ON | FreeType License (FTL), chosen from FreeType's FTL / GPLv2 dual licence; credit below |
| GoogleTest 1.17.0 (tests only, not shipped) | `FetchContent` in the root `CMakeLists.txt` when `N2ENGINE_BUILD_TESTS` is ON | BSD 3-Clause |

## FreeType (optional)

Only builds configured with `-DN2ENGINE_TEXT_FREETYPE=ON` contain FreeType: CMake fetches
FreeType 2.13.3 and links it into the `text` library (see `docs/text.html#freetype`). FreeType is
dual-licensed (the FreeType License or the GNU GPL version 2); N2Engine uses it under the FreeType
License (FTL), which requires this credit in the documentation of any product
that includes it:

> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org).
> All rights reserved.

The full licence is `docs/FTL.TXT` in the FreeType source, also at
<https://gitlab.freedesktop.org/freetype/freetype/-/blob/VER-2-13-3/docs/FTL.TXT>.
The year in the credit is the year of the FreeType version used (the 2024 date of FreeType 2.13.3 is not recorded in this repository);
to be confirmed against the FreeType project's current guidance.
Builds with the option off (the default) contain no FreeType code and don't need this notice.

## OpenAL Soft (LGPL)

N2Engine uses OpenAL Soft 1.24.0 as an unmodified, dynamically linked library. CMake fetches it from
<https://github.com/kcat/openal-soft> at the tag `1.24.0` (root `CMakeLists.txt`, no patch step) and
builds it as a shared library (the CMake build sets no static type; on Linux `deploy/Dockerfile`
copies `libopenal.so*` next to the host; the Windows DLL name is to be confirmed), so it can be replaced by another build of OpenAL Soft. The
corresponding source is at <https://github.com/kcat/openal-soft/tree/1.24.0>; its licence is `COPYING`
in that source tree. OpenAL Soft's source tree also bundles third-party code under its own licences;
see that repository. The build does not set a static library type for OpenAL Soft; changing it to a
static build would change this arrangement and this section would need revisiting.

## PhysX on the web

The web (Emscripten) build compiles PhysX from source instead of downloading an archive. `scripts/web-build-physx.sh`, run by
`cmake/PhysXWeb.cmake`, fetches <https://github.com/NVIDIA-Omniverse/PhysX> at the commit pinned in
`cmake/PhysXWebPin.cmake` (`09ff24f3279b735e672ff27b155cbf49f6296f4d`, tag `107.3-omni-and-physx-5.6.1`, PhysX 5.6.1) and
builds five static, CPU-only libraries (PhysX, PhysXCommon, PhysXFoundation, PhysXExtensions, PhysXPvdSDK) into
`external/PhysX/physx/bin/emscripten.wasm32` (git-ignored), together with a copy of that commit's headers. The source is
modified by six small patches, all in the script and each checked to have applied (the vendored headers in
`external/PhysX/physx/include` are not touched): `-Werror` is removed, two header guards and an overload are extended to
Emscripten (`PxMath.h`, `PxHash.h`), and three platform-glue lines of the foundation sources are changed (`FdUnixThread.cpp`,
`FdTempAllocator.cpp`, `FdUnixMutex.cpp`); docs/web-build.html lists them. PhysX's licence is BSD 3-Clause (Copyright (c)
2008-2025 NVIDIA Corporation), the same as for the native builds above. It requires the copyright notice, the conditions
and the disclaimer to be reproduced in the documentation or other materials that come with a binary distribution: the
build script copies PhysX's `LICENSE.md` next to the libraries, but the web package (`index.wasm` and the other files
the build produces) contains the compiled code and no licence text. Putting this file and the licence texts into
packages is the follow-up already named below. Dependencies: every file under `physx/source` and `physx/include` of the
pinned commit that has a copyright line names NVIDIA, AGEIA or NovodeX, except four Windows resource scripts that are
not built (found with `grep -rIl Copyright source include | xargs grep -L "NVIDIA|AGEIA|NovodeX"`); one file compiled into
PhysXCommon, `GuCookingVolumeIntegration.cpp`, says it is based on public domain code by Brian Mirtich and David Eberly,
which needs no notice. That search found no other third-party notice in the PhysX source the five libraries are built from.

## Not shipped

The Vulkan SDK and loader headers, Mesa lavapipe, the Khronos validation layers and glslang are build
or CI tools only. The Docker runtime image (`deploy/Dockerfile`) does not install them: it installs
only `libvulkan1` and other system libraries from the Ubuntu base image, which carry their own
licences, plus the engine host and the OpenAL Soft library.

Copying this file and the licence texts into the Docker image and release archives is a separate
follow-up; the image does not currently include them.
