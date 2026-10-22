Accepted by the user 2026-10-13:
- Namespace Renderer::Vulkan::ShaderInterface -> Renderer::Gpu::ShaderInterface; LightingBlock, BuildLightingBlock, Detail -> Renderer::Gpu
- VulkanMaterial renamed BlockMaterial (Renderer::Gpu) and moved whole, with its TypeName helper, to renderer/common/BlockMaterial.hpp + new renderer/src/common/BlockMaterial.cpp (no new target; it is a new source in the existing renderer glob): it had no Vulkan-specific part, so Vulkan uses it directly, no derived class
- One namespace alias 'ShaderInterface = Gpu::ShaderInterface' in Renderer::Vulkan (VulkanRenderer.hpp) keeps Vulkan backend code unqualified-spelling unchanged
- Test helper namespaces VulkanTesting::Diff and VulkanTesting::Blend kept unchanged (renaming touches ~20 test files); CrossBackendDiffTests.cpp and BlendReferenceTests.cpp stay in tests/renderer/vulkan (Vulkan-gated glob)
- tests/renderer added to renderer_tests include path; includes are common/CrossBackendDiff.hpp and common/BlendReference.hpp
- Comment-only edits to lit.frag and unlit.vert (renamed header paths); local var vulkanMaterial renamed blockMaterial in VulkanDraw.cpp
- docs/application.html gets one new bullet (PR #188 badge) saying the pieces are shared by GPU backends
