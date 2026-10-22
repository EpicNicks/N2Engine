# Proposal: build and run N2EditorHost (headless, software renderer) on Linux

Read-only research of master (1c248dd) and origin/feat-editor-tls (c079f4f, PR #112). Counts are `git grep` lines, excluding external/ and docs.

## Summary and recommendation
The host is closer to Linux than expected. Sockets, the stdin watcher, env handling and UTF-8 paths already have POSIX branches, and the host runs with no window (`Window::UsesNoWindow`, no glfwInit). What blocks Linux: (a) the build, because renderer/ hard-requires Vulkan, OpenGL and a GLFW that needs X11/Wayland headers; (b) one real runtime bug, no SIGPIPE protection; (c) PhysX has no Linux binaries (the host does not need it).
Recommendation: 4 core PRs, PhysX OFF on Linux, TLS (Mbed TLS) ON, x86_64 only. Keep the existing small `#ifdef`s; do not introduce a socket abstraction library.

## 1. editor-server/ platform surface (src = 6 files, 7404 lines; EditorServer.cpp is 6078)
- EditorServer.cpp: 5 `_WIN32` blocks (headers l.58-77, WSAStartup l.1433, WSACleanup l.1519, setsockopt cast). `NativeSocket`, `CLOSE_SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR_CODE` already have non-Windows definitions (close, -1, errno). Sockets are stored as `int` (`NoSocket = -1`). Calls: accept l.1663, send l.6027, recv l.6043, select l.6065 (100 ms poll loop; FD_SETSIZE 1024 limit is fine for a few editor clients).
- Linux BUG: no MSG_NOSIGNAL and no SIGPIPE ignore anywhere in editor-server/ (grep: 0 hits). A client dropping mid-send kills the host. Fix: MSG_NOSIGNAL on send plus `signal(SIGPIPE, SIG_IGN)` at host start (also covers Mbed TLS writes).
- HostOptions.cpp: 3 `_WIN32` blocks. stdin-EOF watcher: Windows GetStdHandle/ReadFile, POSIX `::read(0, ...)` with EINTR retry already written. `--token-env`: `_dupenv_s`/`_putenv_s` vs getenv/unsetenv, both written.
- Host.cpp: 0 `_WIN32` blocks; `--exit-on-stdin-eof` and `--exit-on-disconnect` are portable. There is no parent-pid option (0 hits). Under systemd/docker stdin is /dev/null (instant EOF), so never pass `--exit-on-stdin-eof` there.
- File watcher polls directory listings (docs/editor.html design-watcher-polling), no ReadDirectoryChangesW: portable. The asset path checker already refuses Windows-illegal names on any OS.
- UTF-8 paths: `Utf8()`/`FromUtf8()` via `u8string`, portable. ProjectFile.cpp has 2 `_WIN32` blocks (transient rename errors; `UserDataBase()` reads `_wdupenv_s(L"APPDATA")`). TO VERIFY in PR B: the non-Windows branch of `UserDataBase()` (XDG/`$HOME`) and its behaviour when `$HOME` is unset (systemd, docker). `--project` gives each project its own user:// folder, so only the shared fallback matters.
- CMake: `ws2_32` is already `$<PLATFORM_ID:Windows>`-guarded; Python3 codegen is optional.
- TLS (PR #112, EditorTls.cpp 787 lines): 3 `_WIN32` sites, all with POSIX branches (socket error/would-block helpers, BIO send/recv, `fs::permissions` 0600 key and 0700 dir when not Windows). `bcrypt` is linked on Windows only; Mbed TLS entropy on Linux uses getrandom. Vendored, ~190 C files under external/mbedtls, option `N2ENGINE_EDITOR_TLS` default OFF.
- Tests that include winsock directly: tests/editor-server/EditorServerHardeningTests.cpp (14 lines), HostOptionsTests.cpp (3), project/EditorProjectTests.cpp (1). All of tests/: `_WIN32` 13, `WIN32` 16, `SOCKET` 4, WSAStartup 1, closesocket 1 (also AudioTests, LoopbackAudioTests, ResourceUUIDNamespaceTests, SerializationRobustnessTests). They need the same shim (a small shared tests header). EditorTlsTests.cpp has 0 Windows hits.

## 2. Libraries the host links, Linux status
Chain: N2EditorServer -> engine, renderer, math; engine -> renderer, math, assetimport, OpenAL, lua, text, (PhysX).
- math: `-msse2 -msse4.1 -mavx` hardcoded for non-MSVC (math/CMakeLists.txt). `_WIN32` 6 lines, all header target-attribute switches with a GCC/Clang path. Consequence: x86_64 with AVX only; arm64 (Graviton) fails to compile.
- text: stb_truetype always, FreeType optional (FetchContent). 1 `WIN32`, 3 MSVC mentions, all CMake runtime-library settings. CPU-only. assetimport: 0 Windows code, CPU-only (ufbx optional). lua: vendored C, `__linux__` already in LuaApplicationBindings.
- OpenAL: openal-soft 1.24.0 via FetchContent, always built. No sound device is needed (`ALSOFT_DRIVERS=null` as in Windows CI). Build may want ALSA/Pulse dev headers; without them it builds only the null/wave backends (acceptable, verify).
- renderer: `find_package(Vulkan REQUIRED)`, `find_package(OpenGL REQUIRED)`, GLFW 3.4 via FetchContent, glad; `VK_USE_PLATFORM_XLIB_KHR` already set for UNIX. One static lib holds opengl/, vulkan/ and software/ (SoftwareRenderer.cpp, DrawOrder.cpp). `--renderer software` still LINKS glfw/glad/Vulkan: engine/src/Window.cpp has 58 GLFW lines, engine/include+src/input has about 220 (InputMapping.hpp alone 144 key constants). So GLFW cannot be dropped cheaply; it is built without any display, and no X server is needed at runtime. A headless-only option is a large refactor, not recommended first.
- PhysX: root CMakeLists.txt forces it OFF on Linux ("binaries not yet configured", l.77); the release asset is win.x86_64.vc143 only. PhysX 5 is BSD-3 and can be built for Linux, but making and hosting the binaries is its own project. Without it the engine reports physics Disabled (Windows CI already has a PhysX OFF leg); play mode has no physics on Linux.
- MSVC-isms: `__declspec` 0; `#pragma comment(lib)` 1 (inside `_WIN32`); `/bigobj`, `/MP`, `/arch:AVX` are all MSVC/WIN32 guarded. `_WIN32` counts outside editor-server and tests: engine/src 5 (Logger, ProjectFile x2, LuaApplicationBindings, plus a windows.h include under `__has_include`), math 6, renderer 1; windows.h: engine 2, editor-server 1, tests 1; `WIN32` in CMakeLists: root 4, engine 2, renderer 3, text/assetimport/lua_project/test_project 1 each. No `<windows.h>` outside guards.
- Toolchain: C++23 (`std::expected`, `std::format`) needs GCC 14 or Clang 17+ (`std::format` is GCC 13+, ubuntu-24.04 has gcc 13/14). Nobody has compiled these ~7000 server lines plus the engine on GCC/Clang yet: expect a few missing includes and warnings (guess 5-30 lines). CRLF: protocol job already passes on ubuntu, so generated files are LF-stable.

## 3. CMake and CI
- Options: `N2ENGINE_BUILD_TESTS` ON, `N2ENGINE_USE_PHYSX` ON (forced OFF on Linux), `N2ENGINE_TEXT_FREETYPE` OFF, `N2ENGINE_MODEL_UFBX` OFF, `N2ENGINE_EDITOR_TLS` OFF (PR #112). FetchContent at configure (network): glfw 3.4, openal-soft 1.24.0, googletest 1.17.0.
- `test_project` (find_package(OpenGL REQUIRED)) and `lua_project` are always built; use `cmake --build --target` for the host and test targets, or add an option later.
- ubuntu-24.04 configure: `apt install libvulkan-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev python3`, then `cmake -S . -B build -DN2ENGINE_USE_PHYSX=OFF -DN2ENGINE_EDITOR_TLS=ON -DN2ENGINE_TEXT_FREETYPE=ON -DN2ENGINE_MODEL_UFBX=ON`. Single-config, so the host is `build/bin/N2EditorHost` (Windows job uses `build/bin/<Config>/N2EditorHost.exe`).
- tests.yml today: `protocol` job (ubuntu, no build) and `windows` matrix (3 legs, 90 min). The host smoke test is a long pwsh step (`.exe`, `Resolve-Path`, `Start-Process -NoNewWindow`, ready line, stdin pipes, token env); pwsh exists on ubuntu runners but it must be parameterised, or ported to Python.
- An ubuntu leg runs: configure, build, `ctest --output-on-failure` with `ALSOFT_DRIVERS=null`, then the smoke test. No GPU is needed (the Windows job already runs GPU-less); to verify: tests/renderer and the default-renderer fallback when there is no X display (glfwInit fails; the Windows smoke test relies on the same fallback).

## 4. Plan (PR-sized) and risks
1. PR A, Linux build leg (1-2 PRs): ubuntu job in tests.yml with the apt list above, PhysX OFF, fix first compile errors, winsock shim in the 3 test files, ctest green. Do after #112 merges so TLS is on from day one.
2. PR B, Linux runtime fixes (1 PR): MSG_NOSIGNAL/SIGPIPE, verify `UserDataBase()` and the `$HOME`-less case, a test that a client dropping mid-frame does not kill the host.
3. PR C, smoke test on Linux (1 PR): port the host smoke test (create, ready line, TCP session, stdin EOF, TLS handshake, certificate reuse, 0600 key permissions) to one cross-platform Python script run on both OSes.
4. PR D, docs and deploy recipe (1 PR): docs/editor.html section, systemd unit, Dockerfile, firewall and TLS guidance (`--token-env`, cert dir).
5. Optional PR E (1-3 PRs, separate decision): Linux PhysX binaries, or arm64 (conditional `-mavx`).
Total: 4-5 core PRs, 6-8 with extras.

Biggest risks: (1) renderer/GLFW/Vulkan/OpenGL are hard build dependencies, so even a headless image needs X11/Vulkan dev packages, and the first GCC/Clang compile of server plus engine is unproven; (2) SIGPIPE kills the host when a client drops, and a publicly bound host without TLS and token is exposed; (3) no Linux PhysX binaries and math hardcodes `-mavx`, so Linux hosts have no physics and no arm64.

## Questions for the user
1. Is physics on the Linux host needed (play mode)? Recommended: PhysX OFF for now.
2. x86_64 only (recommended), or arm64/Graviton too?
3. Bind 127.0.0.1 only behind an SSH tunnel or reverse proxy, or listen publicly? Public means `--tls` plus a token required, and I did not confirm a bind-address option exists (check HostOptions).
4. Should the ubuntu leg be a required check, and should #112 (TLS) merge first?
5. Port the smoke test to Python (one script for both OSes) instead of extending pwsh?
6. Accept installing X11/Vulkan dev packages for now, or invest later in a headless-only CMake option?
7. Container image or plain VM with systemd? Decides what PR D contains.
