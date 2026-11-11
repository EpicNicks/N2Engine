# Build targets: design

Item: `orchestrator/items/build-targets.json` (gated). Requested by the user on 2026-11-10: "I would like for the engine to be
able to allow the user to select a build target in the engine editor UI like windows, linux, android, ios, web, and maybe other
console toolchains in the future like switch".

**Status: proposal, 2026-11-10, amended the same day after review (stage order, unmerged input code, headless backend,
physics per target, remote hosts, saved key codes, web size). Nothing decided yet: section 6 lists the decisions. No stage is
approved.**

## 1. Goal

From the editor, a user picks a target (Windows, Linux, Android, iOS, web; later a console) and presses Build, or Build and Run.
The editor reports what the target needs that is missing, builds a package for that target, and for a device target installs
and starts it with the game's log in the editor console. The same build is one command line for CI.

A console target must be addable later **without its code living in this repository**: console SDKs are under NDA.

## 2. What exists today (verified on master `47fa3a9e` and the Triangle-Like integration branch, 2026-11-10)

- **Targets that work:** Windows and Linux natively (`lua_project`, which already runs any project folder given its path), and
  the web (`N2WebPlayer` and its `web-package` target, docs/web-build.html). macOS has no build. Android and iOS have nothing.
- **Platform code is spread through the engine, with no platform layer.** GLFW is called directly in `engine/src/Window.cpp`
  (60 references), the input system (`InputBinding.cpp` 31, `Mouse.cpp` 12, `Keyboard.cpp` 10, `InputSystem.cpp` 8) and in
  every native renderer (`SoftwareRenderer.cpp:1046,1203` presents through a GL context; `VulkanSwapchain.cpp:262-274` and
  `VulkanRenderer.cpp:541-551` create the surface and instance extensions through GLFW; the OpenGL renderer needs a desktop GL
  3.3 core context). **Key and button codes are GLFW's:** `engine/include/engine/input/InputMapping.hpp` has 144 references.
- **The web build fakes GLFW.** `external/web-stubs` is a no-op GLFW plus a `<generator>` for libc++. It works for one extra
  platform and will not scale to four.
- **Part of the input code is not on master yet.** `engine/src/input/Keyboard.cpp`, `Input::Touchscreen`, gamepad rumble and
  composites, and their web player hooks are on the Triangle-Like branches (`feat-lua-key-text-input`, `feat-input-touch`,
  `feat-input-gamepad-rumble`, `feat-input-gamepad-composites`, merged together in `triangle-like-integration`), with no PR as
  of this date. The counts above that include `Keyboard.cpp` are from that branch. Stage 5, which rewrites this code, waits for
  them to merge.
- **Saved data holds GLFW key numbers.** A binding serializes its key as the raw number (`"key"` in
  `engine/src/input/InputBinding.cpp`), so scenes, action maps and input settings saved today carry GLFW codes. The replay
  files do not: `tests/golden/replays/*.n2replay.json` record abstract per-tick input, not key codes.
- **Several programs run with no window:** the editor host (also in Docker), `n2server`, the test suites, and the golden replay
  and determinism CI legs (`Window` has a windowless mode for them).
- **Physics modules are per platform.** PhysX comes as pinned prebuilts for Windows, Linux and the web (`cmake/PhysX*Hashes.cmake`,
  `cmake/PhysXWeb*.cmake`); Jolt and Box2D are built from source. A mobile target has no PhysX build.
- **The web player is the model for a non-GLFW host.** `web/player/WebPlayer.cpp` runs the engine windowless, calls
  `Application::RunFrame` from the host's frame callback, and feeds input through `InjectedKeys`, `Mouse::InjectPointer` and
  (on the Triangle-Like branch) `Input::Touchscreen`. Android and iOS hosts have the same shape.
- **Files are read straight from disk** through `std::filesystem` and streams (about 195 call sites in `engine/src`).
  `user://` comes from `APPDATA` or XDG and falls back to `"."` (`engine/src/io/ProjectFile.cpp:400-430`). There is no virtual
  filesystem and no pack format; the web packs the folder with Emscripten's `--preload-file`.
- **Per-game module lists exist** (`build.modules` in `project.n2proj`, `cmake/N2EngineModules.cmake`, `-DN2ENGINE_PROJECT`).
- **The editor has no build command.** The Electron frontend (`N2EngineWebFrontend`) talks to the host through
  `editor-server/protocol/protocol.json`, which has nothing for building or exporting. The host can be remote (Docker,
  `deploy/`, the frontend's `ssh-tunnel.ts`). The frontend already starts local processes (`src/main/host-launcher.ts`).
- **libc++ has no `std::generator`.** The engine's coroutines use it, so every libc++ toolchain (Emscripten, the Android NDK,
  Apple clang) needs the shim in `external/web-stubs/include/generator`.
- **Dawn native is a CI harness only** (`cmake/DawnPrebuilt.cmake`, design decision W8: "never a shipped renderer"), Linux
  x86_64 prebuilt.

## 3. Architecture

Five layers. The stages (section 5) do not build them bottom up: the build tool, settings and editor panel (3.4, 3.5) come
first, since they work for today's targets without the platform layer, which only new platforms need.

### 3.1 Platform layer (`engine/platform/`)

An engine interface for everything GLFW does today, with one backend per platform:

| Interface | Covers |
| --- | --- |
| `IPlatformHost` | Create and destroy the window or surface, size and DPI, safe-area insets, fullscreen, cursor, clipboard, text input on and off (soft keyboard), keep the screen on |
| Lifecycle events | Paused, resumed, surface lost, surface recreated, low memory, quit requested, back button |
| Loop ownership | The engine owns the loop (desktop: `Application::Run`) or the OS calls `Application::RunFrame` (web, Android, iOS) |
| Input sources | Keys, pointer, touches, gamepads, rumble, all in **engine codes**, not GLFW's |
| Native surface | A handle the renderer can use: HWND, X11 or Wayland, `ANativeWindow`, `CAMetalLayer`, a canvas |
| Paths | The read-only project root (`res://`), the writable `user://` folder, a temporary folder |

Backends: `desktop-glfw` (Windows, Linux, macOS), `headless`, `web`, `android`, `ios`, and private console backends. The web
backend replaces `external/web-stubs`' fake GLFW. GLFW stays, but only inside `desktop-glfw`. The `headless` backend (no
window, no surface, input only by injection, paths from the command line or the environment) replaces today's windowless mode
for the editor host, `n2server`, the tests and the replay legs; it is part of stage 6, not a later addition, since CI and the
remote editor run on it.

### 3.2 Renderer surfaces and backends per target

Renderers take a native surface from the platform layer instead of a `GLFWwindow*`. The software renderer gets a "present"
callback per platform (GL blit on desktop, canvas on the web, EGL/GLES blit on Android, a Metal or CoreGraphics blit on iOS),
so **every target can draw with the software renderer first.**

| Target | Hardware renderer |
| --- | --- |
| Windows, Linux | Vulkan, OpenGL (exist) |
| Web | WebGPU through emdawnwebgpu (exists) |
| Android | Vulkan (`VK_KHR_android_surface`), or GLES 3 (a port of the GL 3.3 renderer), or WebGPU on Dawn native: decision D2 |
| iOS, macOS | No native Vulkan. Metal, MoltenVK, or WebGPU on Dawn native: decision D2 |
| Consoles | The vendor's API, in the private backend |

Dawn native runs on Metal, Vulkan and D3D12, and the engine already has a WebGPU backend, so one backend could cover web, iOS
and Android. That would revisit W8.

### 3.3 Virtual filesystem and packs

`ResourceLoader` and the other readers go through mount points: a folder (the editor and development builds), a **pack file**
(shipped builds), and the platform's own bundle (Android's APK through `AAssetManager`, iOS's app bundle). Writes go only to
`user://`. A **cook step** turns a project folder into a pack: the module-list check that already exists, the assets, `.import`,
`project.n2proj`, and a manifest of sizes and hashes (the web's `manifest.json` generalised). The web package then packs the
same pack instead of a preloaded folder, which also opens the way to persistent web saves (IDBFS on `user://`). The pack
reader adds code to the web player, so stage 4 measures the package against the web size budget; the web may use an
uncompressed (stored) pack if inflate costs more than it saves.

### 3.4 Players and the build tool

- **One player per platform:** `players/desktop` (today's `lua_project` main, generalised), `players/web` (today's `web/`),
  `players/android` (a Gradle template around GameActivity and the engine `.so`), `players/ios` (a CMake-generated Xcode
  project). A player opens the pack beside it or inside its bundle.
- **Two build modes (decision D3).** *Prebuilt:* the target's player, built once per engine version, plus the game's pack.
  Fast, and needs no compiler for Windows, Linux or the web. *From source:* builds the player with the game's module list
  (engine stripping) or native plugins. Games are Lua and data, so prebuilt is the default.
- **Target descriptors** (`targets/<name>/target.json`): the CMake preset or toolchain file, the toolchains required and how to
  detect them, the host systems that can build it, the packaging steps, the outputs, and the deploy steps. The engine reads
  descriptors from `targets/` and from extra folders listed in the user's settings, which is where private console targets go.
- **`n2build`**, a command-line tool and the one way to build: `n2build --project DIR --target android --config release --out DIR`,
  plus `n2build doctor --target android` (what is missing and how to fix it) and `n2build run` (install, start, stream the log).
  It prints JSON progress lines for the editor. CI, the editor and a person at a terminal all call it.
- **Before packs exist** (stages 1 to 3), `n2build` packages what works today: the desktop player beside a copy of the project
  folder, and the web package with its preload. Stage 4 swaps both to a pack without changing the command line.
- **Physics per target.** `doctor` and the build check the project's `build.modules` against what the target can build
  (decision D8): today PhysX only on Windows, Linux and the web. A project that needs PhysX on a target without it fails with a
  clear message, not a link error.

Host-system limits, which `doctor` reports:

| Target | Can be built on |
| --- | --- |
| Windows | Windows |
| Linux | Linux; Windows through WSL or a container |
| Web | any (emsdk) |
| Android | any (SDK, NDK, JDK) |
| iOS, macOS | macOS only (Xcode) |
| Consoles | the vendor's approved setup |

### 3.5 Project settings and the editor

- `project.n2proj` gains `build.targets`: per target, the app id, display name, version and build number, icons, orientation,
  the renderer, module-list overrides (the web keeps its tight size budget), and **references** to signing material. Keystores,
  passwords and provisioning profiles live in the user's local settings, never in the project.
- The host exposes the targets through `GetProjectInfo` and `SetProjectSettings` (a protocol change).
- The frontend gets a **Build Settings** panel: the target list with each target's status from `n2build doctor`, the target's
  settings, Build and Build and Run, a progress bar and the build log, and for device targets a device picker, with the
  device's log in the editor console. Where the build runs is decision D1.
- **Remote hosts.** With the host in Docker or over SSH, the project lives on the host, not beside the Electron app, so a build
  run by the Electron main process (D1 (a)) has nothing to build. Until D1 (b) exists, the panel says so for a remote host and
  shows the targets and settings without a Build button.

## 4. Per-target work

- **Windows, Linux:** move onto the platform layer, the pack and `n2build`; package as a folder or zip (Linux: tarball, later
  AppImage). Mostly done already.
- **Web:** move onto the platform layer and the pack; the package exists.
- **Android:** the platform backend (GameActivity input, lifecycle, `ANativeWindow`, `getFilesDir()`, `Vibrator` for rumble,
  insets, the back button); openal-soft's AAudio backend; the `<generator>` shim; a renderer per D2; the Gradle template;
  APK for development, AAB for release; signing; `adb install`, start and `logcat`; an emulator smoke in CI; physics per D8
  (Jolt and Box2D from source; PhysX only if built for the NDK).
- **iOS:** the UIKit backend; a renderer per D2; AVAudioSession around OpenAL; the bundle and app container paths; the Xcode
  template and code signing; the simulator for CI and a device for checks; physics per D8. Needs a Mac.
- **macOS (decision D5):** nearly free once iOS's Metal path and the desktop GLFW backend exist.
- **Consoles:** a private repository with a platform backend, a renderer if needed and a target descriptor. Needs a developer
  account and the vendor SDK. The public repository only ships the interface and a sample private target.

## 5. Stages

Each stage is its own item and PR and needs the user's go. The order puts what the user asked for first: stages 1 to 3 give
target selection in the editor for the three targets that already work (Windows, Linux, web) without touching the input,
window or renderer code. The platform layer (stages 5 to 7) is only needed for new platforms, so it comes just before Android.
(Amended 2026-11-10: the first draft ran the platform layer first, which delayed the Build button behind the riskiest stage.)

| Stage | Item | What | Depends on | Size |
| --- | --- | --- | --- | --- |
| 1 | `bt1-n2build-desktop-web` | Target descriptors, `n2build` (build, doctor, run), the desktop player, Windows, Linux and web targets from project folders, the physics-module check per target, CI calls `n2build` | D1, D3, D4 | L |
| 2 | `bt2-project-targets-protocol` | `build.targets` in `project.n2proj`, its validation, `GetProjectInfo` and `SetProjectSettings`, docs | 1 | S |
| 3 | `bt3-fe-build-settings` (frontend) | The Build Settings panel, `n2build` from the Electron main process, progress, log, doctor results; the remote-host case | 2 | M |
| 4 | `bt4-vfs-packs` | Mounts, the pack format and the cook step; native and web players read packs; `user://` as today on desktop and web (mobile paths come with their backends); web size measured | 1, D6 | L |
| 5 | `bt5-engine-input-codes` | Engine key, mouse-button and gamepad codes; GLFW codes only in the GLFW backend; saved GLFW key numbers migrated on load | the Triangle-Like input branches merged | M |
| 6 | `bt6-platform-host` | The platform layer (3.1); `desktop-glfw`, `headless` and `web` backends; `Window.cpp` and the web player moved onto it; `web-stubs`' fake GLFW removed | 5 | L |
| 7 | `bt7-renderer-surfaces` | Renderers take native surfaces; the software renderer's present callback per platform | 6 | M |
| 8 | `bt8-android-player` | Android platform backend, renderer (D2), physics (D8), Gradle template, the `android` target, emulator smoke | 4, 7 | L |
| 9 | `bt9-android-deploy` | Signing, AAB, devices, install and run, `logcat` into the editor | 3, 8 | M |
| 10 | `bt10-ios-player` | iOS backend, renderer (D2), physics (D8), Xcode template, the `ios` target, simulator smoke; macOS if D5 | 4, 7, D7 | L |
| 11 | `bt11-private-targets` | Descriptor discovery outside the repository, the plugin boundary for platform and renderer backends, a sample private target, docs | 1 | M |

**After stage 3, stop and compare.** The editor then builds the three working targets. Whether packs (4) come next or wait
until mobile depends on how stage 1 went.

**Behaviour changes.** Stages 1 to 4 add tools and settings and change nothing about how a game runs. Stages 6 and 7 must not
change behaviour either: the tests, the web smoke, the replay legs and the budgets stay as they are. Stage 5 does change saved
data: GLFW key numbers in scenes, action maps and input settings become engine codes, migrated on load, with a test for a file
saved before it.

Stage 6 is the largest risk: it touches every input and window path, and the headless programs. Stage 5 lands first so that
stage 6 is mechanical.

## 6. Decisions for the user

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| D1 | Where a build runs | (a) the Electron main process runs `n2build` on the user's machine; (b) the host runs it (a `BuildProject` command) | **(a) first.** Toolchains and devices are on the user's machine. A remote host (Docker, SSH) keeps the project on the host, so (a) cannot build there: the panel says so (3.5) until (b) exists for remote hosts and build servers |
| D2 | Hardware renderer on mobile | (a) WebGPU on Dawn native for Android, iOS and macOS (revisits W8); (b) Vulkan on Android, Metal or MoltenVK on Apple; (c) GLES 3 on Android | **(a)**, after a spike: one backend for web and mobile. Every target ships the software renderer first regardless |
| D3 | Player model | (a) prebuilt players plus a pack by default, from source for stripped builds; (b) always from source | **(a)** |
| D4 | `lua_project` | (a) becomes `players/desktop`; (b) stays as a sample and a new player is added | **(a)** |
| D5 | macOS | in scope with iOS, or later | With iOS: little extra once Metal exists |
| D6 | Pack format | (a) zip (stored or deflate) with a manifest; (b) a custom format | **(a)**: tools can inspect it and it is easy to read from an APK |
| D7 | A Mac for iOS | Is one available for builds and device checks? | Needed before stage 10 |
| D8 | Physics on mobile | (a) Jolt and Box2D only (a project needing PhysX cannot target mobile); (b) also build PhysX for the NDK and Apple | **(a)**: both build from source; PhysX for mobile is a large extra build to maintain. Revisit if a game needs it |

## 7. Not verified

- That the engine builds with the Android NDK's clang and libc++ beyond `std::generator` (no NDK build was tried).
- openal-soft 1.24 on Android in this engine's configuration.
- Dawn native's size and start time on mobile, and whether its prebuilt releases cover Android and Apple (they did not on
  Linux arm or Windows when `DawnPrebuilt.cmake` was written; building Dawn from source may be needed).
- How many of the `std::filesystem` call sites assume a real disk (a stage 4 audit).
- The Switch: whether its SDK allows this plugin shape. Only a developer account can tell.
- Which projects and saved files hold GLFW key numbers today (the stage 5 migration needs the full list), and how the editor
  inspector shows a binding's key.
- The size of a zip reader in the web player (stage 4).

## 8. Risks

- **Stage 6 regressions** in input on desktop and web, and in the headless programs. Mitigation: stage 5 first; the
  existing input tests, smoke plans and replay legs run unchanged; a recorded-input replay before and after.
- **Conflicts with the Triangle-Like input work.** Stages 5 and 6 rewrite the code those unmerged branches add. Mitigation:
  stage 5 starts only after they merge (its gate says so).
- **Saved data.** A missed migration in stage 5 would silently rebind keys in old projects. Mitigation: a test loads files
  saved before the stage; the reviewer checks every place a key number is written.
- **Toolchain drift** (NDK, Xcode, emsdk). Mitigation: each descriptor pins versions, `doctor` checks them, CI uses the same
  pins, as the web build already does for emsdk.
- **Secrets.** Signing material must never reach the project file, the host, logs or the protocol. The reviewer checks this
  on stages 2, 3 and 9.
