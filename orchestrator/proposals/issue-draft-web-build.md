# DRAFT issue (not posted): Web build support (WebAssembly)

**Title:** Web build: run N2Engine games in the browser (WebAssembly)

## Summary
Make it possible to build an N2Engine game for the web and run it in a browser tab. The engine is C++23 + CMake with Lua scripting, a software renderer, an OpenGL 3.3 renderer, a Vulkan renderer in progress, OpenAL audio and optional PhysX. The goal of this issue is a **web player** (run a project in the browser), not the editor.

## Toolchain: Emscripten, effectively regardless of the graphics choice
There is no real alternative to Emscripten for C++ with a window, GL and audio in a browser: WASI toolchains (wasi-sdk) have no GPU, window or audio. Even the "modern" route (WebGPU through Dawn or wgpu) is built for the browser with Emscripten. So the real decision is **which renderer backend the web build uses**, not the compiler.

## Renderer options
| Option | What it means | Pros | Cons |
|---|---|---|---|
| **A. WebGL2 backend (recommended v1 target)** | Port the OpenGL renderer to GLES 3.0 / WebGL2 (Emscripten maps GLES3 to WebGL2) | Runs in every modern browser; closest to the existing OpenGL renderer | Today's renderer is desktop **OpenGL 3.3 core** via GLFW + glad; shaders are `#version 330 core` and need a `#version 300 es` variant; glad is not used on the web (Emscripten provides GL entry points) |
| **B. Software renderer to a canvas** | Compile the existing CPU renderer to WASM and blit its framebuffer to a 2D canvas | Zero graphics porting; already has goldens, so it is easy to test under node; good **first milestone** | Slow; its `RenderThread` needs threads or a single-thread fallback |
| **C. WebGPU backend** | A new backend (could share ideas with the Vulkan backend in progress) via Dawn/`emdawnwebgpu` | Modern API, good long-term | Large new backend; browser support still uneven; needs WGSL or SPIR-V to WGSL translation |
| **D. Pixel streaming (not a port)** | Reuse the editor host's existing frame and audio streaming, with a small web viewer through a WebSocket gateway | No engine port at all | Needs a server per player; latency; not a web build |

Suggested path: **B as the first milestone** (proves the toolchain, CI and asset pipeline with no GPU work), then **A** for real performance. C is a possible later project.

## Engine-specific blockers (checked in the code)
- **Window and input:** GLFW (`engine/src/Window.cpp`, input system). Emscripten has a GLFW3 port/contrib port, so this is probably small, but the context hints (`GLFW_OPENGL_CORE_PROFILE`, 3.3) must change for the web.
- **OpenGL:** desktop GL 3.3 core + glad (`external/glad`). Needs a GLES3 path: shader dialect, no `glPolygonMode`, no geometry/tessellation (confirm none used), texture format differences, no `GL_FRAMEBUFFER_SRGB`.
- **Threads:** `std::thread`/`std::mutex` appear in the logger, the software renderer's `RenderThread` and the Vulkan renderer. Browser pthreads need `SharedArrayBuffer`, which needs the page to be **cross-origin isolated** (COOP/COEP headers). Decide: require it, or provide a single-threaded fallback.
- **Audio:** OpenAL Soft is fetched via FetchContent. On the web use Emscripten's own OpenAL (WebAudio) instead of building openal-soft. Browsers require a user gesture before audio starts.
- **Physics:** PhysX is optional (`N2ENGINE_USE_PHYSX`). There is no official PhysX WebAssembly build; community WASM builds exist (CPU only, no GPU). Options: ship without physics, a community PhysX WASM build, or a simpler engine-side physics. **Decision needed.**
- **Filesystem:** `std::filesystem` is used widely (33 files). Emscripten provides a virtual filesystem; assets would be preloaded or fetched, and saves would use IndexedDB (IDBFS). Synchronous file loads need rethinking (async fetch or preload).
- **Networking and TLS:** the editor host uses raw TCP sockets and Mbed TLS. Not needed for the player; the web player should exclude `editor-server` (and the editor).
- **Lua / sol2 / FreeType / ufbx / cgltf:** plain C/C++, expected to compile with Emscripten.
- **Size:** WASM + assets over the network; set a size budget and use `-Oz`/`-O3` + `wasm-opt`, compressed hosting.

## Proposed stages
0. **Toolchain + CI smoke:** an Emscripten CMake toolchain path, an `N2ENGINE_PLATFORM_WEB` option, a CI leg using `emsdk` that builds the core libraries (math, text, engine without window/audio/physics) and runs unit tests **under node** (`ctest` with node as the emulator).
1. **Headless engine in WASM:** engine core, scene loading, Lua scripting, software renderer goldens under node.
2. **Software renderer to canvas** + Emscripten window/input (GLFW port): the first playable web build.
3. **Assets and packaging:** preload/virtual FS, async loading, a shell HTML page, hosting notes (headers).
4. **Audio** through Emscripten OpenAL / WebAudio.
5. **WebGL2 renderer backend** (shader variants, GLES3 limits), with cross-backend diff goldens against the software renderer, like the Vulkan work.
6. **Physics decision and integration.**
7. **Threads** (cross-origin isolation or fallback) and performance work.

## Open questions
1. Player only, or is an in-browser editor also a goal? (Recommendation: player only.)
2. Renderer: software first then WebGL2 (recommended), WebGL2 directly, or WebGPU?
3. Physics on the web: none, PhysX WASM, or something else?
4. Minimum browsers and mobile support?
5. Is requiring cross-origin isolation (COOP/COEP) acceptable for hosting?
6. Size and load-time budget?
7. Where would it be hosted/demoed (GitHub Pages cannot set COOP/COEP headers without workarounds)?

## Acceptance for a first version
- CI builds the web target with Emscripten on every PR and runs the headless unit tests under node.
- `lua_project` (or a trimmed demo) runs in a browser with the software renderer: scene loads, scripts run, input works.
- Documented build and hosting instructions (including headers) in `docs/`.
- Native Windows/Linux builds unchanged.

## Verification
CI can verify the build, unit tests under node and software-rendered goldens. Real-browser behaviour (WebGL2, audio, input, performance, mobile) needs a manual check by the maintainer.
