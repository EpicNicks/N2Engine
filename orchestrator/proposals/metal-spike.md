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
- **Not measured yet:** frame rate (MoltenVK against the software renderer, and against Metal if B is built), MoltenVK's size
  in an app, and an iOS app on MoltenVK (static XCFramework, `VK_EXT_metal_surface` from the iOS backend).

The spike's evidence supports the recommendation: the existing renderer runs on MoltenVK as it is, apart from one
documented portability limit with a contained fix.

## The pick

**MoltenVK**, picked by the user on 2026-11-12. The Vulkan renderer becomes the GPU renderer on macOS and iOS through
MoltenVK. What follows, in its own PR: the swizzle limit fixed (one- and two-channel textures stored as RGBA where the device
cannot swizzle), MoltenVK linked into the macOS player and the iOS app, and the Apple backends' Metal surface for it. A
native Metal backend is not planned.
