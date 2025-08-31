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
