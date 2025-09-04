# Vulkan Renderer Notes

## Vulkan Setup Sequence

- **CreateInstance** ✅  
  Must come first — all Vulkan objects depend on the instance.

- **SetupDebugMessenger**  
  Can only happen after the instance is created.  
  Doesn’t depend on anything else, so order is correct here.

- **CreateSurface (needs windowHandle)**  
  Must come after instance.  
  Must come before swap chain creation, because the swap chain depends on it.  
  Can technically happen before or after `PickPhysicalDevice`, but usually you do surface first, so that you can filter physical devices by presentation support.

- **PickPhysicalDevice**  
  Needs the surface (to check presentation support).  
  Must happen before logical device creation.

- **CreateLogicalDevice**  
  Needs the physical device selection.  
  Must be before swap chain creation (swap chain is tied to queues from the device).

- **CreateSwapChain**  
  Requires surface and logical device.  
  Must come before image views.

- **CreateImageViews**  
  Needs swap chain images.

- **CreateRenderPass**  
  Must come before pipeline creation, because pipeline depends on the render pass.

- **CreateDescriptorSetLayout**  
  Independent of render pass or pipeline, but must come before graphics pipeline if the pipeline uses descriptors.  
  Could be moved earlier, but keeping it here is fine.

- **CreateGraphicsPipeline**  
  Needs:  
  - Render pass  
  - Descriptor set layout(s)  
  - Swap chain extent (for viewport/scissor)  
  Order is correct.

- **CreateCommandPool**  
  Needs logical device.  
  Could happen anytime after logical device creation. Doesn’t depend on render pass or pipeline, so it could be moved earlier if desired.

- **CreateDepthResources**  
  Needs swap chain extent and image format.  
  Must happen before framebuffers (since framebuffers attach the depth image).

- **CreateFramebuffers**  
  Needs render pass, swap chain image views, and depth resources.  
  Order correct.

- **CreateUniformBuffers**  
  Needs logical device.  
  Can happen earlier, but leaving it here is fine.  
  Must be before descriptor pool and descriptor set allocation.

- **CreateDescriptorPool**  
  Needs logical device.  
  Can happen any time after that, but must be before descriptor set allocation (not explicitly shown here).

- **CreateCommandBuffers**  
  Needs command pool.  
  Needs framebuffers, pipeline, and render pass to record correct draw commands.  
  Order is correct.

- **CreateSyncObjects**  
  Needs logical device.  
  Doesn’t strictly depend on framebuffers or command buffers, so it could be earlier, but usually it’s done last.

---

## Summary of Strict Order Constraints

- `Instance → Debug Messenger`  
- `Instance → Surface → Physical Device → Logical Device`  
- `Logical Device → Swap Chain → Image Views`  
- `Render Pass → Graphics Pipeline`  
- `Depth Resources → Framebuffers`  
- `Command Pool → Command Buffers`  

Everything else is somewhat flexible.


## Logical Device Creation

Currently set to basic values. Fields which should be configurable:

### High Priority
#### Queue Configuration
- Queue count per family to allow multiple queues for threading
- type of queue family to request other than present queues
- graphics vs compute priorities

#### Device Features
- enabling other features than samplerAnisotropy such as
  - tesselation
  - geometry shaders
  - compute shaders
  - multisampling
  - wide lines
  - etc

#### Extensions
- different device extensions required for different rendering techniques such as
  - ray tracing
  - mesh shaders
  - variable rate shading

#### Sample Config
**Sample Config Type and API signature**
```cpp
struct DeviceConfig {
    std::vector<const char*> extensions;
    VkPhysicalDeviceFeatures features{};
    uint32_t graphicsQueueCount = 1;
    uint32_t presentQueueCount = 1;
    float queuePriority = 1.0f;
    bool enableValidation = false;
};

bool CreateLogicalDevice(const DeviceConfig& config);
```

## Swapchain Configuration Guide

This section outlines the configurable parameters in the `CreateSwapChain()` function and their importance for different rendering scenarios.

### High Priority - Critical for Different Use Cases

### Present Mode Selection

Currently uses `ChooseSwapPresentMode()` but applications may need to force specific modes based on their requirements:

- `VK_PRESENT_MODE_IMMEDIATE_KHR` - No vsync, lowest latency, may tear
- `VK_PRESENT_MODE_FIFO_KHR` - Vsync enabled, guaranteed supported on all implementations
- `VK_PRESENT_MODE_MAILBOX_KHR` - Triple buffering, provides smooth rendering with low latency

Different application types benefit from different present modes. Games typically prefer mailbox or immediate mode for responsiveness, while media applications might prefer FIFO for smooth playback.

### Image Count Configuration

The current implementation uses `minImageCount + 1`, but applications may want explicit control over buffering strategy:

- Exactly 2 images for double buffering (minimal memory usage)
- 3+ images for triple buffering (smoother frame delivery)
- User-specified count with intelligent fallback logic

More images generally provide smoother rendering but consume additional GPU memory and may increase input latency.

### Image Usage Flags

Currently hardcoded to `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, but different rendering techniques require additional usage flags:

- `VK_IMAGE_USAGE_TRANSFER_DST_BIT` for post-processing effects and blits
- `VK_IMAGE_USAGE_STORAGE_BIT` for compute shader access
- `VK_IMAGE_USAGE_SAMPLED_BIT` for sampling swapchain images in shaders

### Medium Priority - Important for Quality and Performance

#### Surface Format Selection

The current `ChooseSwapSurfaceFormat()` logic may not suit all applications. Configurable format preferences enable:

- sRGB vs linear color space handling for proper gamma correction
- HDR format support for high dynamic range content
- Different bit depths (8-bit vs 10-bit vs 16-bit) for color accuracy

#### Composite Alpha Configuration

Currently hardcoded to `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`, but some applications require:

- `VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR` for overlay applications
- `VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR` for special transparency effects
- `VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR` to inherit from surface settings

#### Transform Handling

Currently uses `currentTransform` directly, but applications may need custom transform logic for:

- Mobile device rotation handling
- Multi-monitor setups with different orientations  
- Custom viewport transformations

### Lower Priority - Specialized Use Cases

#### Array Layers

Currently hardcoded to 1, but should be configurable for:

- VR/AR applications requiring stereoscopic rendering (2 layers)
- Multi-view rendering techniques
- Layered rendering for special effects

#### Clipping Behavior

Currently set to `VK_TRUE`, but some scenarios benefit from `VK_FALSE`:

- Debugging and development tools
- Applications that need to render outside viewport bounds
- Special overlay or windowing effects

#### Recommended Configuration Structure

```cpp
struct SwapchainConfig {
    VkPresentModeKHR preferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    uint32_t preferredImageCount = 3; // 0 = auto (min + 1)
    VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkFormat preferredFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR preferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    uint32_t arrayLayers = 1;
    bool clipped = true;
};
```

### Implementation Priority

Present mode and image count configuration should be implemented first, as they have the most direct impact on application performance, input latency, and visual quality. Different application types (games, productivity software, media players) often require different trade-offs in these areas.

## Image View Configuration Guide

This section outlines the configurable parameters in the `CreateImageView()` function and their importance for different texture and rendering scenarios.

### High Priority - Common Use Cases

#### View Type Configuration

Currently hardcoded to `VK_IMAGE_VIEW_TYPE_2D`, but different texture types require different view types:

- `VK_IMAGE_VIEW_TYPE_1D` for one-dimensional textures and gradients
- `VK_IMAGE_VIEW_TYPE_3D` for volumetric textures and 3D data
- `VK_IMAGE_VIEW_TYPE_CUBE` for environment maps and skyboxes
- `VK_IMAGE_VIEW_TYPE_2D_ARRAY` for texture atlases and layered rendering
- `VK_IMAGE_VIEW_TYPE_CUBE_ARRAY` for arrays of cubemaps

Different rendering techniques depend on appropriate view types. Skybox rendering requires cube views, while shadow mapping often uses 2D array views for multiple cascade levels.

#### Mip Level Range

Currently uses `baseMipLevel = 0, levelCount = 1`, but many textures require mipmap support:

- Views of specific mip levels for different levels of detail
- Partial mip chains for progressive loading or streaming
- Individual mip levels for debug visualization or special effects

Mipmapped textures are essential for texture filtering quality and performance, especially in 3D rendering where objects appear at varying distances.

#### Array Layer Range

Currently uses `baseArrayLayer = 0, layerCount = 1`, but many modern rendering techniques require array support:

- Texture arrays for efficient batch rendering
- Cubemap faces accessed as array layers
- Stereo rendering with separate layers for left/right eyes
- Shadow cascade arrays for directional light shadows

Array textures are fundamental for modern GPU-efficient rendering patterns and specialized techniques like VR rendering.

### Medium Priority - Advanced Rendering Techniques

#### Component Swizzling

Currently uses default identity swizzling, but format reinterpretation often requires custom component mapping:

- Channel reordering for different format interpretations (RGB to BGR)
- Single-channel textures mapped to different color components
- Special effects requiring component manipulation
- Compatibility with different texture format conventions

Component swizzling enables efficient texture reuse and format compatibility without requiring texture data copies.

#### Format Reinterpretation

While format is configurable via parameter, advanced use cases may need compatible format views:

- Aliasing between different numeric interpretations (uint to float)
- Compressed format views for specific shader requirements  
- Format compatibility for cross-API texture sharing

Format reinterpretation allows efficient texture data reuse across different rendering passes and shader requirements.

### Lower Priority - Specialized Features

#### Aspect Mask Flexibility

Currently passed as parameter but could benefit from more structured configuration:

- Combined depth-stencil aspect handling
- Plane-specific views for multi-planar formats
- Specialized aspect combinations for advanced formats

### Recommended Configuration Structure

```cpp
struct ImageViewConfig {
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED; // use image's format if undefined
    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    
    // Subresource range
    uint32_t baseMipLevel = 0;
    uint32_t levelCount = 1;
    uint32_t baseArrayLayer = 0;  
    uint32_t layerCount = 1;
    
    // Component swizzling
    VkComponentSwizzle r = VK_COMPONENT_SWIZZLE_IDENTITY;
    VkComponentSwizzle g = VK_COMPONENT_SWIZZLE_IDENTITY;
    VkComponentSwizzle b = VK_COMPONENT_SWIZZLE_IDENTITY;
    VkComponentSwizzle a = VK_COMPONENT_SWIZZLE_IDENTITY;
};

VkImageView CreateImageView(VkImage image, const ImageViewConfig& config);
```

### Alternative Dual-Function Approach

Maintain both simple and advanced interfaces for different use cases:

```cpp
// Simple version for common cases
VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

// Full version for advanced cases  
VkImageView CreateImageView(VkImage image, const ImageViewConfig& config);
```

### Implementation Priority

View type and subresource range configuration should be implemented first, as they enable fundamental texture types like cubemaps, texture arrays, and mipmapped textures. These features are essential for most modern rendering techniques and cannot be easily worked around without proper image view configuration.

## Render Pass Configuration Guide

This section outlines the configurable parameters in the `CreateRenderPass()` function and their importance for different rendering techniques and optimization strategies.

### High Priority - Essential for Different Rendering Techniques

#### Attachment Configuration

Currently hardcoded to color + depth attachments, but different rendering passes require various attachment configurations:

- **G-Buffer passes** require multiple color attachments (albedo, normals, material properties)
- **Shadow mapping** needs only depth attachment, no color
- **Post-processing** may need different color formats or no depth
- **HDR rendering** requires floating-point color formats instead of swapchain format
- **Multisampled rendering** needs configurable sample counts beyond `VK_SAMPLE_COUNT_1_BIT`

Modern rendering pipelines often use specialized passes with vastly different attachment requirements, making flexible configuration essential.

#### Load and Store Operations

Currently uses `VK_ATTACHMENT_LOAD_OP_CLEAR` for both attachments, but optimization requires different strategies:

- **VK_ATTACHMENT_LOAD_OP_LOAD** for incremental rendering or multi-pass techniques
- **VK_ATTACHMENT_LOAD_OP_DONT_CARE** for temporary attachments to improve performance
- **VK_ATTACHMENT_STORE_OP_DONT_CARE** for depth buffers that won't be read later
- **VK_ATTACHMENT_STORE_OP_STORE** only when attachment data is needed in subsequent passes

Load/store operation selection significantly impacts mobile GPU performance and memory bandwidth usage.

#### Layout Transitions

Currently uses fixed layouts (`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` for color), but different use cases require different transitions:

- **VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL** for textures used in subsequent passes
- **VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL** for attachments used as copy sources
- **VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL** for intermediate render targets
- **VK_IMAGE_LAYOUT_GENERAL** for compute shader access or complex usage patterns

Proper layout management is critical for performance and correctness in multi-pass rendering.

### Medium Priority - Advanced Rendering Features

#### Multiple Subpass Support

Currently creates a single subpass, but advanced techniques benefit from multiple subpasses:

- **Deferred shading** with geometry and lighting subpasses
- **Tile-based rendering** optimizations on mobile GPUs
- **Order-independent transparency** techniques
- **Multi-pass post-processing** within the same render pass

Subpass dependencies and scheduling can provide significant performance benefits on tile-based architectures.

#### Multisampling Configuration

Currently hardcoded to `VK_SAMPLE_COUNT_1_BIT`, but anti-aliasing requires configurable sampling:

- Different MSAA levels (2x, 4x, 8x) for quality/performance trade-offs
- Per-attachment sample count configuration
- Resolve attachment configuration for MSAA targets
- Sample shading and coverage options

#### Stencil Operations

Currently uses `VK_ATTACHMENT_LOAD_OP_DONT_CARE` for stencil, but many techniques require stencil:

- **Shadow volumes** for accurate soft shadows
- **Stencil-based optimizations** for light culling
- **Outline rendering** and selection highlighting
- **Portal rendering** and visibility techniques

### Lower Priority - Specialized Use Cases

#### Input Attachments

Not currently supported but useful for:

- **Programmable blending** on mobile GPUs
- **Bandwidth-efficient post-processing**
- **Tile-based deferred shading** optimizations

#### Preserve Attachments

For bandwidth optimization on mobile architectures where certain attachment data should be preserved between subpasses without explicit storage.

### Recommended Configuration Structure

```cpp
struct AttachmentConfig {
    VkFormat format;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout;
};

struct SubpassConfig {
    std::vector<uint32_t> colorAttachments;
    std::optional<uint32_t> depthStencilAttachment;
    std::vector<uint32_t> inputAttachments;
    std::vector<uint32_t> resolveAttachments;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

struct RenderPassConfig {
    std::vector<AttachmentConfig> attachments;
    std::vector<SubpassConfig> subpasses;
    std::vector<VkSubpassDependency> dependencies;
};

bool CreateRenderPass(const RenderPassConfig& config);
```

### Alternative Incremental Approach

Start with parameterized attachment configuration while maintaining the simple interface:

```cpp
struct SimpleRenderPassConfig {
    std::vector<VkFormat> colorFormats;
    std::optional<VkFormat> depthFormat;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool clearColor = true;
    bool storeColor = true;
    bool clearDepth = true;
    VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

bool CreateRenderPass(const SimpleRenderPassConfig& config);
```

### Implementation Priority

Attachment configuration (formats, count, load/store operations) should be implemented first, as these enable the most common rendering variations like G-Buffer passes, shadow mapping, and HDR rendering. Multiple color attachments and configurable load/store operations provide the foundation for most modern rendering techniques.