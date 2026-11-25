#pragma once

// PRIVATE to renderer/src/vulkan/: the post-processing and render-texture state of the Vulkan backend (issue #445,
// VulkanPostProcess.cpp). Made lazily on the first post-process; freed by Shutdown.

#include <cstdint>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"

namespace Renderer::Vulkan
{
    struct PostState
    {
        // The passes into the half-size bloom images (colour only, finishing in SHADER_READ_ONLY_OPTIMAL) and the pass that
        // resumes the frame after a post-process or capture (the frame's attachments, loaded: compatible with the frame's
        // own pass, so the built-in pipelines and the framebuffer work in it)
        VkRenderPass offscreenPass = VK_NULL_HANDLE;
        VkRenderPass resumePass = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE; // binding 0 the source, 1 the bloom (combined image samplers)
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE; // the set and 48 bytes of push constants (Common::PostProcessParams)
        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        VkPipeline offscreenPipeline = VK_NULL_HANDLE; // for offscreenPass
        VkPipeline framePipeline = VK_NULL_HANDLE;     // for the frame's pass (no depth test or write)
        VkSampler sampler = VK_NULL_HANDLE;            // linear, clamped to the edge
        bool failed = false;                           // the pipelines could not be made: post-processing is off

        // Sized to the frame: the copy of the colour target and the two bloom images with their framebuffers
        VulkanImage scene;
        VulkanImage bloomA;
        VulkanImage bloomB;
        VkFramebuffer bloomAFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer bloomBFramebuffer = VK_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        /// The textures CreateRenderTexture made (still alive)
        std::vector<const Common::ITexture*> renderTextures;
    };
}
