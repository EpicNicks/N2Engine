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

RESULTS (filled in from the `metal-spike` job of the stage 13 PR)

## The pick

PICK (the user's decision)
