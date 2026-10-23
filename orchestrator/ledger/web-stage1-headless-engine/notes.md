Accepted by the user 2026-10-13:
- GLFW and OpenAL are not built: external/web-stubs downloads their headers (FetchContent, URL archives, headers only) and defines no-op C stand-ins for the functions the engine calls (the plan said exclude GLFW/OpenAL by #if/CMake; their public headers are included everywhere, so a link-level stand-in was the small guarded option)
- Added a <generator> replacement header (external/web-stubs/include/generator) because libc++ has no std::generator; it defers to the toolchain header when present
- Web builds compile and link with -fwasm-exceptions (engine/sol2/json/gtest throw and catch); changes the stage 0 flags for math/text too
- Test executables link with -sNODERAWFS=1 and a --pre-js (tests/web/node-env.js) that copies node process.env, so tests see HOME/TMPDIR
- ResourceLoader.cpp: web-only file-time conversion (no clock_cast in libc++) and an unconditional static_cast<size_t> of a file size (identity natively, narrowing error on wasm32)
- Tests: added assetimport_tests to the web job (not asked for); engine exclusion list is only the 2 audio test files, plus GTEST_SKIPs for the audio-asset fixture, the 8-thread logger test and one musl-atoll glTF assertion (#ifndef web)
- CI web build step uses -- -k 0 so one run shows every compile error
- Application.GetPlatform() in Lua still answers Unknown on web (not changed)
- gh pr edit fails on this token (GraphQL scopes): PR body updated through the REST API instead
