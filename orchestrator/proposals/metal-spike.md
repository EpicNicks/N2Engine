# Build targets stage 13: Metal, the spike

Item: `bt13-metal-renderer` (a later goal of `orchestrator/proposals/build-targets-design.md`, decision D2). The plan says:
"a spike that compares MoltenVK with native Metal first; the user picks". This is the spike. Nothing here is decided until
the user picks; the code that follows the pick is a separate PR.

## The question

iOS and macOS draw with the software renderer today (stage 10): the frame is drawn on the CPU and shown as the view's layer
contents on iOS, or through OpenGL on macOS (OpenGL is deprecated there). A GPU renderer on Apple platforms is either:

- **A. MoltenVK:** the engine's existing Vulkan renderer (about 6,300 lines in `renderer/src/vulkan` and its headers, plus
  the GLSL shaders under `renderer/shaders/vulkan`) runs on MoltenVK. MoltenVK is the Khronos implementation of Vulkan on
  Metal (Apache 2.0). It is a library that translates Vulkan calls to Metal and SPIR-V shaders to Metal Shading Language at
  run time.
- **B. Native Metal:** a fourth GPU backend, written against Metal directly (through Apple's metal-cpp headers, so the
  engine stays C++), with its shaders in Metal Shading Language.
- **C. Both, in order:** A now, B later if the measurements say MoltenVK costs too much.

## What each costs

| | A. MoltenVK | B. Native Metal |
|---|---|---|
| Renderer code | none new: the Vulkan renderer already enables `VK_KHR_portability_subset` on a portability device, and this branch makes the instance ask for `VK_KHR_portability_enumeration` (without it, Vulkan loaders since 1.3.216 hide MoltenVK and no GPU is found) | a new backend of the Vulkan renderer's size: device, swapchain (`CAMetalLayer`), pipelines, buffers, textures, the render target, the frame loop, its tests |
| Shaders | the existing GLSL, compiled to SPIR-V as today; MoltenVK translates it | Metal Shading Language: written by hand, or translated at build time from SPIR-V (SPIRV-Cross) or from the WebGPU backend's WGSL (naga, which CI already runs for WGSL validation) |
| Surface | a `CAMetalLayer` through `VK_EXT_metal_surface` (the platform layer already has `NativeSurface::Kind::MetalLayer`) | the same `CAMetalLayer`, used directly |
| iOS | no system Vulkan loader: MoltenVK is linked into the app (its static XCFramework), with no loader in between | Metal is a system framework |
| Shipping | MoltenVK's library in the app bundle (its size to be measured in the follow-up) | nothing extra |
| Speed | an extra translation layer: shader translation when pipelines are made (cached), a small cost per call. For this engine's draw counts it is unlikely to matter; it is measured, not assumed (follow-up) | the best Apple can do |
| Features | Vulkan Portability limits (`VK_KHR_portability_subset`, for example no triangle fans, which Metal lacks); whether the renderer uses anything on the list is what the CI job below shows | all of Metal, nothing more |
| Debugging | Xcode's GPU frame capture works (MoltenVK issues Metal), with Vulkan names one step removed | Xcode's tools directly |
| Maintenance | one GPU renderer for Windows, Linux, Android (stage 12) and Apple | Vulkan, Metal, OpenGL and WebGPU, each kept in step with the others |

## What the spike measures

The `metal-spike` CI job (`.github/workflows/tests.yml`) builds the Vulkan renderer for macOS, against Homebrew's MoltenVK,
the Vulkan loader and headers, and glslang (`-DN2ENGINE_APPLE_MOLTENVK=ON`). It then runs the Vulkan test suite on
MoltenVK: `renderer_tests --gtest_filter=Vulkan*`, the same tests the Linux jobs run on lavapipe. They cover the device, the
swapchain on a headless surface, draws and read-back, and the stage 12 surface loss and recreation. The job never fails;
it reports what built and which tests passed, failed or were skipped, in its summary. That is the evidence for or against
A: if the suite passes, the renderer runs on MoltenVK as it is.

Not measured by the spike, and needed after the pick:
- the frame rate against the software renderer on the simulator and a Mac (and against B, if B is built);
- MoltenVK's size in an app;
- a run inside an iOS app (needs the static XCFramework and `VK_EXT_metal_surface` from the iOS backend).

## Recommendation

**A (MoltenVK), if the CI suite passes,** with C as the path should the measurements disagree. The engine has one mature
GPU renderer with tests on every CI platform. MoltenVK reuses all of it, on the same code path Android now takes (stage 12).
A native Metal backend would roughly double the GPU renderer code that has to stay in step, before it is shown to be
needed.

## Results

From the `metal-spike` job of PR #350 (macos-15 runner, 2026-11-12):

- **Built:** the Vulkan renderer and `renderer_tests` built for macOS against Homebrew's MoltenVK 1.4.2, Vulkan loader
  1.4.363, headers and glslang, with no source change beyond the portability enumeration below.
- **The device:** the loader found MoltenVK's "Apple Paravirtual device" (the runner is a virtual machine; an integrated
  GPU type, Vulkan 1.4, 7168 MiB, `VK_EXT_headless_surface` and `VK_EXT_metal_surface` offered).
- **The suite, with the validation layer on** (`renderer_tests --gtest_filter=Vulkan*`, the same tests the Linux jobs run on
  lavapipe): 309 tests ran, **262 passed, 46 failed, 1 skipped**.
  - **All 46 failures have one cause, a Vulkan Portability limit.** MoltenVK reports `imageViewFormatSwizzle` as false, and
    the renderer gives its one- and two-channel textures (the text atlas among them) a component swizzle in their image
    views (`renderer/src/vulkan/VulkanResources.cpp`, the R and RG views). The validation layer flags each of those views
    (`VkPhysicalDevicePortabilitySubsetFeaturesKHR::imageViewFormatSwizzle`). That is 19 text-shader tests, 23 text golden
    tests across three suites (10, 7 and 6), and 4 resource tests.
  - The fix is small and does not change other platforms: where the device lacks `imageViewFormatSwizzle`, store such
    textures as RGBA (expand on upload) or apply the swizzle in the shader.
  - The one skip is the no-compiled-shaders fallback test, which skips on every platform with compiled shaders.
- **Not measured by the spike:** frame rate (MoltenVK against the software renderer, and against Metal if B is built),
  MoltenVK's size in an app, and an iOS app on MoltenVK (static XCFramework, `VK_EXT_metal_surface` from the iOS backend).
  The iOS app on MoltenVK came with PR #353; the size and the frame rate are measured below ("Measured").

The spike's evidence supports the recommendation: the existing renderer runs on MoltenVK as it is, apart from one
documented portability limit with a contained fix.

## Measured: MoltenVK's size and speed (follow-up, PR #366)

The `Apple measurements` workflow (`.github/workflows/apple-measure.yml`, script `scripts/apple-measure.py`; it runs on
demand and on a pull request that changes it, and gates nothing) builds `lua_project` in **Release** on `macos-15` runners,
without and with MoltenVK v1.4.2, and times it. First run:
[43525490291](https://github.com/EpicNicks/N2Engine/actions/runs/43525490291) (PR #366, 2026-11-13).

**These are a virtual machine's numbers.** The runners are VMs with Apple's paravirtual GPU, and the iOS simulator's Metal
runs on that GPU through the Mac. They say how the pieces compare on one machine, not what a Mac or an iPhone does.

Size (MiB; "stripped" is the executable with debug and local symbols stripped, measured on a copy; "zipped" is the whole
package in one deflate zip, about what a download costs):

| Build | Package | Zipped | Executable | Stripped |
|---|---|---|---|---|
| macOS player, without MoltenVK | 29.78 | 6.78 | 28.52 | 19.93 |
| macOS player, with MoltenVK | 35.30 | 8.45 | 28.96 | 20.39 |
| iOS simulator app, without MoltenVK | 26.29 | 5.58 | 26.25 | 18.17 |
| iOS simulator app, with MoltenVK | 31.13 | 7.07 | 31.10 | 21.75 |

- **macOS:** MoltenVK itself is `libMoltenVK.dylib` beside the player, 5.08 MiB; the player's own Vulkan renderer adds
  0.45 MiB (stripped). The package grows 5.53 MiB, 1.68 MiB zipped.
- **iOS:** MoltenVK is linked into the executable: MoltenVK and the Vulkan renderer together add 3.58 MiB (stripped), so
  MoltenVK's own part is about 3.1 MiB once the renderer's 0.45 MiB measured on macOS is taken off. The app grows 4.84 MiB,
  1.49 MiB zipped. The static library before linking: 6.80 MiB for a device (`ios-arm64`), 13.48 MiB for the simulator
  (arm64 and x86_64); the linker keeps only what is used. A device build's size is not measured.

Speed (the same scene, `lua_project`'s `scene.lua`):

| Run | Frame time | Frame rate | How |
|---|---|---|---|
| macOS VM, software renderer, offscreen 1280x720 (`--headless`) | 5.75 ms | 174 fps | (330 - 30 frames) wall time / 300, median of 3 (5.50, 6.47, 5.75 ms) |
| macOS VM, Vulkan on MoltenVK, window, vsync off (`N2_VULKAN_VSYNC=0`) | 16.52 ms | 60.5 fps | the same (4.45, 17.99, 16.52 ms) |
| macOS VM, Vulkan on MoltenVK, window, vsync on | 19.43 ms | 51.5 fps | the same (21.28, 19.43, 18.69 ms) |
| iOS simulator, software renderer | 18.2 ms | 54.8 fps | the player's five-second lines over 60 s, median of 16 (32.8 to 58.8 fps) |
| iOS simulator, Vulkan on MoltenVK | 48.3 ms | 20.7 fps | the same, median of 11 (10.9 to 24.7 fps) |

What they show, and what they do not:

- **On the macOS VM the Vulkan window runs at about the display's rate** (60 Hz) even with vsync off, in two of three
  repeats; the third ran at 4.45 ms (225 fps). Presenting through the VM's window server paces it, so the frame's own cost
  is not what this measures. The software renderer's 174 fps is offscreen and presents nothing (the runner has no OpenGL
  context, so neither OpenGL nor the software renderer's window can run there): the rows are not a like-for-like race.
  They show that both are well inside a 60 Hz frame for this scene on this VM.
- **In the simulator, MoltenVK is slower than the software renderer** (about 21 against 55 fps; the display link caps both
  at 60). The simulator forwards Metal to the Mac's GPU, and there the app runs MoltenVK without Metal argument buffers
  (`docs/apple.html#moltenvk-ios`); neither is so on a device. This is not evidence for or against MoltenVK on an iPhone:
  only a device run can say (`docs/apple.html#checklist` item 6, which needs a developer account).
- The Tests workflow's `apple` jobs build Debug, which is why they log about 3 fps in the simulator; Release runs at 55 there.
- The workflow takes about 46 macOS runner minutes (the two macOS jobs, 8 and 10 minutes, in parallel with the iOS job, 28)
  and under a minute on Linux for the report. It does not run on ordinary pull requests.

Still not measured: an iPhone (a device build's size, its frame rate), a Mac's own GPU, and a native Metal renderer (not
built; see the pick).

## The pick

**MoltenVK**, picked by the user on 2026-11-12. The Vulkan renderer becomes the GPU renderer on macOS and iOS through
MoltenVK. What follows, in its own PR: the swizzle limit fixed (one- and two-channel textures stored as RGBA where the device
cannot swizzle), MoltenVK linked into the macOS player and the iOS app, and the Apple backends' Metal surface for it. A
native Metal backend is not planned.
