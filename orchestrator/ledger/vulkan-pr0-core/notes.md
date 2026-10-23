Accepted by the user 2026-10-14:
- Added renderer/include/renderer/common/RendererConfig.hpp (defaults N2ENGINE_RENDERER_VULKAN to 1) and guarded Window.hpp/Window.cpp with it; the VULKAN backend then fails window creation with a clear message when compiled out
- Initialize with a zero width or height returns false (InitStatus::Failed) instead of clamping
- Initialize with a window handle succeeds and ignores the handle (frames render offscreen, Present is a silent no-op); previously Vulkan init always failed
- Validation default: off in Release, on in Debug when the layer is installed (missing layer only warns); explicit enable (N2_VULKAN_VALIDATION=1 or SetValidationMode(Enabled)) fails Initialize with InitStatus::ValidationUnavailable when the layer is missing
- Added public test/diagnostic API on VulkanRenderer: SetValidationMode, GetInitStatus/GetInitMessage, GetValidationStats/Messages, GetDeviceName/Type, GetRenderWidth/Height (IRenderer unchanged)
- Resize/OnResize recreate the offscreen target until SetRenderTargetSize fixes the size (then they are ignored), mirroring OpenGL
- ReadFramebuffer resamples nearest (pixel centres) when asked for a size other than the target's, like the software renderer
- ICD discovered by glob lvp_icd*.json in a CI step writing VK_DRIVER_FILES to GITHUB_ENV (not hard-coded per arch); package names mesa-vulkan-drivers vulkan-validationlayers glslang-tools worked on noble; glslang-tools and the validation packages are also in the Docker build stage though nothing there uses them yet
- Added N2_VULKAN_EXPECT_CPU_DEVICE=1 (CI only) so a green Linux leg proves the tests ran on lavapipe, besides N2_REQUIRE_VULKAN=1
- Merged origin/master into the branch to resolve a docs/testing.html conflict (PR conflicted, so CI had not run)
- Review fixes after the first review: Present logs ONE warning per renderer instance (swapchain/present is stage F), a canary test injects a synthetic error through vkSubmitDebugUtilsMessageEXT (public test hooks SubmitDebugMessageForTest/ResetValidationStats), docs filter *Vulkan*, stale comments updated (EditorServer.cpp/hpp comment-only; the allowed paths were widened to those two files), the linux GCC 14 leg now builds with -DN2ENGINE_RENDERER_VULKAN=OFF (and no Vulkan packages / env vars) to prove the compile-out path; master was merged again (clean).
Head 7fa510f = master merge + docs fix (low finding). Needs CI + fresh re-confirm review, then ratify --merge.
