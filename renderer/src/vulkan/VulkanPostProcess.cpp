#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"

#include "VulkanDraw.hpp"
#include "VulkanPostProcess.hpp"
#include "VulkanResources.hpp"

// Post-processing and render textures on Vulkan (issue #445). Everything is recorded into the open frame's command buffer, in
// submission order with its draws:
//
// ApplyPostProcess: the frame's render pass ends (its colour attachment finishes in TRANSFER_SRC_OPTIMAL, its depth is
// stored), the colour target is copied into PostState::scene, the bright pass and the blur passes run in offscreenPass on the
// two half-size bloom images, and the composite is drawn in resumePass, which loads the frame's colour and depth again. The
// passes use the frame's flipped viewport convention, so post.vert reads every source the right way up.
//
// CaptureFrame: the pass ends, the colour target is blitted (linear, scaled, upside down so the texture's first row is the
// bottom of the frame) into the render texture, and the pass resumes.
//
// resumePass is compatible with the frame's pass (same formats, one subpass), so the built-in pipelines and the frame's
// framebuffer are used in it unchanged.

#ifndef N2ENGINE_VULKAN_HAS_SPIRV
#define N2ENGINE_VULKAN_HAS_SPIRV 0
#endif

#if N2ENGINE_VULKAN_HAS_SPIRV
#include "vulkan_shaders/post_frag_spirv.h"
#include "vulkan_shaders/post_vert_spirv.h"
#endif

using namespace Renderer::Vulkan;
using Renderer::Common::ITexture;
using Renderer::Common::PostPassMode;
using Renderer::Common::PostProcessParams;
using Renderer::Common::PostProcessSettings;

namespace
{
    constexpr VkPipelineStageFlags AttachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags AttachmentWrites =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkImageMemoryBarrier ImageBarrier(const VkImage image, const VkImageLayout oldLayout, const VkImageLayout newLayout,
                                      const VkAccessFlags srcAccess, const VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        return barrier;
    }

    void Destroy(VkDevice device, VkPipeline& pipeline)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }

    void Destroy(VkDevice device, VkFramebuffer& framebuffer)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            framebuffer = VK_NULL_HANDLE;
        }
    }

    void Destroy(VkDevice device, VkRenderPass& pass)
    {
        if (pass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(device, pass, nullptr);
            pass = VK_NULL_HANDLE;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::EnsurePostPipelinesLocked()
{
#if N2ENGINE_VULKAN_HAS_SPIRV
    PostState& post = *_post;
    if (post.framePipeline != VK_NULL_HANDLE)
    {
        return true;
    }
    if (post.failed || _device == VK_NULL_HANDLE || _renderPass == VK_NULL_HANDLE)
    {
        return false;
    }
    const auto fail = [&](const char* what)
    {
        std::cerr << "Vulkan renderer: " << what << "; post-processing and captures are off" << std::endl;
        post.failed = true;
        return false;
    };

    // The bloom images' pass: written whole (DONT_CARE), then sampled
    {
        VkAttachmentDescription attachment{};
        attachment.format = ColorFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        std::array<VkSubpassDependency, 2> dependencies{};
        // In: earlier reads of the image (a previous pass sampled it) and writes to it finish first
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        // Out: the writes are visible to the next pass's fragment shader
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        if (vkCreateRenderPass(_device, &info, nullptr, &post.offscreenPass) != VK_SUCCESS)
        {
            post.offscreenPass = VK_NULL_HANDLE;
            return fail("could not make the post-processing render pass");
        }
    }

    // The frame's pass again, loading what the frame drew (colour from TRANSFER_SRC, where the frame's pass and the copies
    // left it; depth as the frame's pass stored it)
    {
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = ColorFormat;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1].format = _depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        std::array<VkSubpassDependency, 2> dependencies{};
        // In: the copies and blits that read the colour target, the bloom passes and the earlier part of the frame finish first
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask =
            AttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].srcAccessMask = AttachmentWrites | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependencies[0].dstStageMask = AttachmentStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstAccessMask = AttachmentWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        // Out: as the frame's pass, to the transfers that read the colour target
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = AttachmentStages;
        dependencies[1].srcAccessMask = AttachmentWrites;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        if (vkCreateRenderPass(_device, &info, nullptr, &post.resumePass) != VK_SUCCESS)
        {
            post.resumePass = VK_NULL_HANDLE;
            return fail("could not make the resume render pass");
        }
    }

    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &info, nullptr, &post.setLayout) != VK_SUCCESS)
        {
            post.setLayout = VK_NULL_HANDLE;
            return fail("could not make the post-processing descriptor set layout");
        }
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(PostProcessParams);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &post.setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &post.pipelineLayout) != VK_SUCCESS)
        {
            post.pipelineLayout = VK_NULL_HANDLE;
            return fail("could not make the post-processing pipeline layout");
        }
    }

    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        if (vkCreateSampler(_device, &info, nullptr, &post.sampler) != VK_SUCCESS)
        {
            post.sampler = VK_NULL_HANDLE;
            return fail("could not make the post-processing sampler");
        }
    }

    const auto makeModule = [&](const std::uint32_t* words, const std::size_t bytes, VkShaderModule& module)
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode = words;
        if (vkCreateShaderModule(_device, &info, nullptr, &module) != VK_SUCCESS)
        {
            module = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    if (!makeModule(kSpirv_post_vert, sizeof(kSpirv_post_vert), post.vertexModule) ||
        !makeModule(kSpirv_post_frag, sizeof(kSpirv_post_frag), post.fragmentModule))
    {
        return fail("could not make the post-processing shader modules");
    }

    const auto makePipeline = [&](const VkRenderPass pass, VkPipeline& pipeline)
    {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = post.vertexModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = post.fragmentModule;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = post.pipelineLayout;
        info.renderPass = pass;
        info.subpass = 0;
        if (vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    if (!makePipeline(post.offscreenPass, post.offscreenPipeline) || !makePipeline(post.resumePass, post.framePipeline))
    {
        Destroy(_device, post.offscreenPipeline);
        Destroy(_device, post.framePipeline);
        return fail("could not make the post-processing pipelines");
    }
    return true;
#else
    return false;
#endif
}

bool VulkanRenderer::EnsurePostTargetsLocked(const uint32_t width, const uint32_t height)
{
    PostState& post = *_post;
    if (post.scene.image != VK_NULL_HANDLE && post.width == width && post.height == height)
    {
        return true;
    }
    if (post.scene.image != VK_NULL_HANDLE)
    {
        // Frames in flight may still read the old images (the frame being recorded does not yet)
        vkDeviceWaitIdle(_device);
        DestroyPostTargetsLocked();
    }
    const uint32_t bloomWidth = Common::BloomExtent(width);
    const uint32_t bloomHeight = Common::BloomExtent(height);
    const auto makeFramebuffer = [&](const VulkanImage& image, VkFramebuffer& framebuffer)
    {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = post.offscreenPass;
        info.attachmentCount = 1;
        info.pAttachments = &image.view;
        info.width = bloomWidth;
        info.height = bloomHeight;
        info.layers = 1;
        if (vkCreateFramebuffer(_device, &info, nullptr, &framebuffer) != VK_SUCCESS)
        {
            framebuffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    const bool ok =
        CreateImage(width, height, ColorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, post.scene) &&
        CreateImage(bloomWidth, bloomHeight, ColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, post.bloomA) &&
        CreateImage(bloomWidth, bloomHeight, ColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, post.bloomB) &&
        makeFramebuffer(post.bloomA, post.bloomAFramebuffer) && makeFramebuffer(post.bloomB, post.bloomBFramebuffer);
    if (!ok)
    {
        LogOnce("could not make the post-processing images; post-processing is skipped");
        DestroyPostTargetsLocked();
        return false;
    }
    post.width = width;
    post.height = height;
    return true;
}

void VulkanRenderer::DestroyPostTargetsLocked()
{
    PostState& post = *_post;
    Destroy(_device, post.bloomAFramebuffer);
    Destroy(_device, post.bloomBFramebuffer);
    DestroyImage(post.scene);
    DestroyImage(post.bloomA);
    DestroyImage(post.bloomB);
    post.width = 0;
    post.height = 0;
}

void VulkanRenderer::DestroyPostLocked()
{
    PostState& post = *_post;
    if (_device != VK_NULL_HANDLE)
    {
        DestroyPostTargetsLocked();
        Destroy(_device, post.offscreenPipeline);
        Destroy(_device, post.framePipeline);
        if (post.vertexModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(_device, post.vertexModule, nullptr);
        }
        if (post.fragmentModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(_device, post.fragmentModule, nullptr);
        }
        if (post.sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(_device, post.sampler, nullptr);
        }
        if (post.pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(_device, post.pipelineLayout, nullptr);
        }
        if (post.setLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(_device, post.setLayout, nullptr);
        }
        Destroy(_device, post.offscreenPass);
        Destroy(_device, post.resumePass);
    }
    post = PostState{};
}

// ---------------------------------------------------------------------------------------------------------------------
// The frame's pass, suspended and resumed
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::SetFlippedViewportLocked(const VkCommandBuffer commandBuffer, const uint32_t width,
                                              const uint32_t height) const
{
    // As BeginFrame's: a negative height keeps OpenGL's y-up clip space (NDC y +1 is the image's first row)
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(height);
    viewport.width = static_cast<float>(width);
    viewport.height = -static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {width, height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanRenderer::SuspendFramePassLocked(const VkCommandBuffer commandBuffer)
{
    // The frame's pass (or a resume pass) leaves the colour target in TRANSFER_SRC_OPTIMAL and stores the depth
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanRenderer::ResumeFramePassLocked(const VkCommandBuffer commandBuffer)
{
    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = _post->resumePass;
    passInfo.framebuffer = _framebuffer;
    passInfo.renderArea.extent = {_targetWidth, _targetHeight};
    vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetFlippedViewportLocked(commandBuffer, _targetWidth, _targetHeight);
}

// ---------------------------------------------------------------------------------------------------------------------
// Post-processing
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::ApplyPostProcess(const PostProcessSettings& input)
{
    const PostProcessSettings settings = PostProcessSettings::Sanitized(input);
    if (!settings.IsActive())
    {
        return;
    }
    std::lock_guard lock(_mutex);
    if (!_frameOpen || _deviceLost || !EnsurePostPipelinesLocked() || !EnsurePostTargetsLocked(_targetWidth, _targetHeight))
    {
        return;
    }
    PostState& post = *_post;
    const VkCommandBuffer commandBuffer = _frames[_currentFrame].commandBuffer;

    // The descriptor sets of the passes (from the frame slot's pools, reset when the slot's frame begins)
    const auto makeSet = [&](const VkImageView source, const VkImageView bloom) -> VkDescriptorSet
    {
        const VkDescriptorSet set = AllocateSet(_currentFrame, post.setLayout);
        if (set == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }
        std::array<VkDescriptorImageInfo, 2> images{};
        images[0] = {post.sampler, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {post.sampler, bloom, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return set;
    };
    const bool bloom = settings.BloomActive();
    const bool blur = settings.BlurActive();
    const bool halves = bloom || blur; // the half-size images are drawn
    const VkDescriptorSet sceneSet = makeSet(post.scene.view, post.scene.view);
    const VkDescriptorSet bloomASet = halves ? makeSet(post.bloomA.view, post.bloomA.view) : VK_NULL_HANDLE;
    const VkDescriptorSet bloomBSet = halves ? makeSet(post.bloomB.view, post.bloomB.view) : VK_NULL_HANDLE;
    const VkDescriptorSet compositeSet = makeSet(post.scene.view, bloom ? post.bloomA.view : post.scene.view);
    const VkDescriptorSet mixSet = blur ? makeSet(post.scene.view, post.bloomA.view) : VK_NULL_HANDLE;
    if (sceneSet == VK_NULL_HANDLE || compositeSet == VK_NULL_HANDLE ||
        (halves && (bloomASet == VK_NULL_HANDLE || bloomBSet == VK_NULL_HANDLE)) || (blur && mixSet == VK_NULL_HANDLE))
    {
        LogOnce("could not allocate the post-processing descriptor sets; post-processing is skipped");
        return;
    }


    // The copy of the frame: whatever read it last (an earlier composite) finishes before it is overwritten
    const auto copyFrame = [&]
    {
        VkImageMemoryBarrier toCopy = ImageBarrier(post.scene.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toCopy);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {_targetWidth, _targetHeight, 1};
        vkCmdCopyImage(commandBuffer, _color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post.scene.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toRead = ImageBarrier(post.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                   VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toRead);
    };

    const auto drawPass = [&](const VkPipeline pipeline, const VkDescriptorSet set, const PostProcessParams& params)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, post.pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commandBuffer, post.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostProcessParams),
                           &params);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    };

    // A half-size image from the copy (the first pass given), then `iterations` pairs of blur passes; the result is in
    // bloomA
    const auto halfPasses = [&](const PostProcessParams& first, const int iterations)
    {
        const uint32_t bloomWidth = Common::BloomExtent(_targetWidth);
        const uint32_t bloomHeight = Common::BloomExtent(_targetHeight);
        const auto offscreen = [&](const VkFramebuffer framebuffer, const VkDescriptorSet set, const PostProcessParams& params)
        {
            VkRenderPassBeginInfo passInfo{};
            passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            passInfo.renderPass = post.offscreenPass;
            passInfo.framebuffer = framebuffer;
            passInfo.renderArea.extent = {bloomWidth, bloomHeight};
            vkCmdBeginRenderPass(commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
            SetFlippedViewportLocked(commandBuffer, bloomWidth, bloomHeight);
            drawPass(post.offscreenPipeline, set, params);
            vkCmdEndRenderPass(commandBuffer);
        };
        offscreen(post.bloomAFramebuffer, sceneSet, first);
        for (int pass = 0; pass < iterations; ++pass)
        {
            const float spread = static_cast<float>(pass + 1);
            PostProcessParams params = PostProcessParams::For(PostPassMode::Blur, settings);
            params.b[0] = spread / static_cast<float>(bloomWidth);
            params.b[1] = 0.0f;
            offscreen(post.bloomBFramebuffer, bloomASet, params);
            params.b[0] = 0.0f;
            params.b[1] = spread / static_cast<float>(bloomHeight);
            offscreen(post.bloomAFramebuffer, bloomBSet, params);
        }
    };

    if (blur)
    {
        SuspendFramePassLocked(commandBuffer);
        copyFrame();
        halfPasses(PostProcessParams::For(PostPassMode::Copy, settings), settings.blurIterations);
        ResumeFramePassLocked(commandBuffer);
        drawPass(post.framePipeline, mixSet, PostProcessParams::For(PostPassMode::Mix, settings));
    }
    if (settings.WarpActive())
    {
        SuspendFramePassLocked(commandBuffer);
        copyFrame();
        ResumeFramePassLocked(commandBuffer);
        PostProcessParams params = PostProcessParams::For(PostPassMode::Warp, settings);
        params.b[1] = 1.0f; // the image's first row is the top of the frame
        drawPass(post.framePipeline, sceneSet, params);
    }
    if (settings.CompositeActive())
    {
        SuspendFramePassLocked(commandBuffer);
        copyFrame();
        if (bloom)
        {
            halfPasses(PostProcessParams::For(PostPassMode::Bright, settings), settings.bloomIterations);
        }
        ResumeFramePassLocked(commandBuffer);
        drawPass(post.framePipeline, compositeSet, PostProcessParams::For(PostPassMode::Composite, settings));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Render textures
// ---------------------------------------------------------------------------------------------------------------------

ITexture* VulkanRenderer::CreateRenderTexture(const uint32_t width, const uint32_t height)
{
    ITexture* texture = Common::IRenderer::CreateRenderTexture(width, height);
    if (texture == nullptr)
    {
        return nullptr;
    }
    std::lock_guard lock(_mutex);
    const auto it = _resources->textures.find(texture);
    if (it == _resources->textures.end())
    {
        return nullptr;
    }
    VulkanTexture& vulkanTexture = *it->second;
    if (_imageViewSwizzle)
    {
        // A capture is opaque: the view reads alpha as 1 (the frame's alpha is under 1 where alpha-blended draws left it).
        // The texture is new, so nothing uses its first view yet
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vulkanTexture.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vulkanTexture.format;
        viewInfo.components = vulkanTexture.swizzle;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, vulkanTexture.mipLevels, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(_device, &viewInfo, nullptr, &view) == VK_SUCCESS)
        {
            vkDestroyImageView(_device, vulkanTexture.view, nullptr);
            vulkanTexture.view = view;
            vulkanTexture.swizzle = viewInfo.components;
        }
    }
    _post->renderTextures.push_back(texture);
    return texture;
}

bool VulkanRenderer::CaptureFrame(ITexture* target)
{
    if (target == nullptr)
    {
        return false;
    }
    std::lock_guard lock(_mutex);
    // The resume pass is made with the post-processing pipelines
    if (!_frameOpen || _deviceLost || std::ranges::find(_post->renderTextures, target) == _post->renderTextures.end() ||
        !EnsurePostPipelinesLocked())
    {
        return false;
    }
    const auto it = _resources->textures.find(target);
    if (it == _resources->textures.end())
    {
        return false;
    }
    const VulkanTexture& texture = *it->second;
    const VkCommandBuffer commandBuffer = _frames[_currentFrame].commandBuffer;

    SuspendFramePassLocked(commandBuffer);
    // Earlier draws of the frame that sampled the texture finish before it is overwritten
    VkImageMemoryBarrier toWrite =
        ImageBarrier(texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toWrite);
    // The colour target's first row is the top of the frame; the texture's is the bottom (v = 0): the source's y runs from
    // the last row up
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, static_cast<int32_t>(_targetHeight), 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(_targetWidth), 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(texture.width), static_cast<int32_t>(texture.height), 1};
    vkCmdBlitImage(commandBuffer, _color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    VkImageMemoryBarrier toRead =
        ImageBarrier(texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toRead);
    ResumeFramePassLocked(commandBuffer);
    return true;
}
