# Third-party notices

N2Engine itself is MIT-licensed (see `LICENSE.md`). This file lists the third-party code and data the
engine builds with, where each comes from and its licence, and holds the notices that some licences
require in a product's documentation. Each component's full licence text is in its own files (vendored
code: the header or licence file under `external/`; fetched code: its source, downloaded at configure
time into the build tree).

## Vendored (in this repository)

| Component | Path | Licence |
|---|---|---|
| stb_truetype 1.26 | `external/stb_truetype/` | MIT or public domain (Unlicense), at your choice |
| stb_vorbis 1.22 | `external/stb_vorbis/` | MIT or public domain (Unlicense), at your choice |
| stb_image 2.30 (nothings/stb commit `2c980bb`), with stb_image_write 1.16 for the tests only | `external/stb_image/` | MIT or public domain (Unlicense), at your choice |
| cgltf 1.15 (jkuhlmann/cgltf tag `v1.15`, which bundles jsmn) | `external/cgltf/` | MIT (`external/cgltf/LICENSE`; jsmn is MIT too) |
| MikkTSpace (mmikk/MikkTSpace commit `3e895b4`, 2020-03-25; tangent generation for normal maps) | `external/mikktspace/` | zlib (`external/mikktspace/LICENSE`, and the notice at the top of both files) |
| dr_libs (dr_wav, dr_flac, dr_mp3) | `external/dr_libs/` | Public domain (Unlicense) or MIT-0, at your choice |
| Lua 5.4 | `external/lua/` | MIT |
| sol2 | `external/sol2/` | MIT |
| nlohmann/json | `external/nlohmann/` | MIT |
| TinySHA1 | `external/TinySHA1/` | ISC-style permissive licence (in the header) |
| glad (generated OpenGL loader) | `external/glad/` | Generated code; see its headers, including the Khronos notice in `external/glad/include/KHR/` |
| Noto Sans Regular 2.015 (subset; the default font) | `engine/assets/fonts/` | SIL Open Font License 1.1 (`engine/assets/fonts/OFL.txt`) |

## Fetched or downloaded at configure time

| Component | Where it comes from | Licence |
|---|---|---|
| GLFW 3.4 | `FetchContent` in `renderer/CMakeLists.txt` | zlib/libpng |
| OpenAL Soft 1.24.0 | `FetchContent` in the root `CMakeLists.txt` | GNU LGPL (see its `COPYING`); ship it in a form that lets users replace it, such as its DLL |
| NVIDIA PhysX (binaries) | Downloaded from this repository's `physx-dependencies` release into `external/PhysX/` when `N2ENGINE_USE_PHYSX` is ON | BSD 3-Clause |
| FreeType 2.13.3 (optional) | `FetchContent` in `text/CMakeLists.txt` when `N2ENGINE_TEXT_FREETYPE` is ON | FreeType License (FTL); credit below |
| GoogleTest 1.17.0 (tests only, not shipped) | `FetchContent` in the root `CMakeLists.txt` when `N2ENGINE_BUILD_TESTS` is ON | BSD 3-Clause |

## FreeType (optional)

Only builds configured with `-DN2ENGINE_TEXT_FREETYPE=ON` contain FreeType: CMake fetches
FreeType 2.13.3 and links it into the `text` library (see `docs/text.html#freetype`). FreeType is used
under the FreeType License (FTL), which requires this credit in the documentation of any product
that includes it:

> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org).
> All rights reserved.

The full licence is `docs/FTL.TXT` in the FreeType source, also at
<https://gitlab.freedesktop.org/freetype/freetype/-/blob/VER-2-13-3/docs/FTL.TXT>.
Builds with the option off (the default) contain no FreeType code and don't need this notice.
