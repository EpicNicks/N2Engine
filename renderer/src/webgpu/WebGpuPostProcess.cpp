#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"
#include "WebGpuInternal.hpp"
#include <renderer/webgpu/WgslSources.hpp>

// Post-processing and render textures on WebGPU (issue #445). Everything is encoded into the open frame's command encoder, in
// order with its draws:
//
// ApplyPostProcess: the frame's render pass ends, the colour target is copied into PostResources::scene, the bright pass and
// the blur passes run as passes of their own on the two half-size bloom textures, and the composite is drawn in a new frame
// pass that loads the colour and depth the frame had (BeginPass(true)).
//
// CaptureFrame: the pass ends, a pass of its own draws the colour target into the render texture (mode Copy, flipY, so the
// texture's first row is the bottom of the frame, alpha 1), and the frame pass resumes, loading.
//
// Each pass's parameters (Common::PostProcessParams) take a slot of the frame's uniform ring, which EndFrame writes before the
// submit, like the draws' blocks.

namespace Renderer::WebGpu
{
    using Common::PostPassMode;
    using Common::PostProcessParams;
    using Common::PostProcessSettings;

    namespace
    {
        /// A texture of the frame's colour format that a pass renders to and a later pass samples (and, for the copy of the
        /// frame, that a copy writes)
        bool MakePostTexture(WebGpuRenderer::State& state, const std::uint32_t width, const std::uint32_t height,
                             const WGPUTextureUsage usage, const char* label, TextureRef& texture, TextureViewRef& view)
        {
            WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
            descriptor.label = ToView(label);
            descriptor.usage = usage | WGPUTextureUsage_TextureBinding;
            descriptor.dimension = WGPUTextureDimension_2D;
            descriptor.size = {width, height, 1};
            descriptor.format = WebGpuRenderer::GetColorFormat();
            texture.Reset(wgpuDeviceCreateTexture(state.device.Get(), &descriptor));
            if (!texture)
            {
                return false;
            }
            view.Reset(wgpuTextureCreateView(texture.Get(), nullptr));
            return static_cast<bool>(view);
        }
    }

    bool WebGpuRenderer::EnsurePostLocked()
    {
        State& state = *_state;
        PostResources& post = state.post;
        if (post.failed || !state.device)
        {
            return false;
        }
        if (!post.framePipeline)
        {
            const auto fail = [&](const char* what)
            {
                LogOnce(std::string(what) + "; post-processing and captures are off");
                post.failed = true;
                return false;
            };

            WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
            source.code = WGPUStringView{Wgsl::kPostWgsl.data(), Wgsl::kPostWgsl.size()};
            WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            moduleDescriptor.label = ToView("N2Engine post-processing");
            moduleDescriptor.nextInChain = &source.chain;
            post.module.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &moduleDescriptor));
            if (!post.module)
            {
                return fail("could not create the post-processing module");
            }

            std::array<WGPUBindGroupLayoutEntry, 4> entries;
            for (WGPUBindGroupLayoutEntry& entry : entries)
            {
                entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            }
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entries[0].buffer.type = WGPUBufferBindingType_Uniform;
            entries[0].buffer.hasDynamicOffset = WGPU_FALSE;
            entries[0].buffer.minBindingSize = sizeof(PostProcessParams);
            for (std::uint32_t i = 1; i <= 2; ++i)
            {
                entries[i].binding = i;
                entries[i].visibility = WGPUShaderStage_Fragment;
                entries[i].texture.sampleType = WGPUTextureSampleType_Float;
                entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
                entries[i].texture.multisampled = WGPU_FALSE;
            }
            entries[3].binding = 3;
            entries[3].visibility = WGPUShaderStage_Fragment;
            entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            layoutDescriptor.label = ToView("N2Engine post-processing group");
            layoutDescriptor.entryCount = entries.size();
            layoutDescriptor.entries = entries.data();
            post.groupLayout.Reset(wgpuDeviceCreateBindGroupLayout(state.device.Get(), &layoutDescriptor));
            if (!post.groupLayout)
            {
                return fail("could not create the post-processing bind group layout");
            }
            const WGPUBindGroupLayout groupLayout = post.groupLayout.Get();
            WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pipelineLayoutDescriptor.label = ToView("N2Engine post-processing pipeline layout");
            pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
            pipelineLayoutDescriptor.bindGroupLayouts = &groupLayout;
            post.pipelineLayout.Reset(wgpuDeviceCreatePipelineLayout(state.device.Get(), &pipelineLayoutDescriptor));
            if (!post.pipelineLayout)
            {
                return fail("could not create the post-processing pipeline layout");
            }

            WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
            samplerDescriptor.label = ToView("N2Engine post-processing sampler");
            samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
            samplerDescriptor.magFilter = WGPUFilterMode_Linear;
            samplerDescriptor.minFilter = WGPUFilterMode_Linear;
            samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            samplerDescriptor.lodMinClamp = 0.0f;
            samplerDescriptor.lodMaxClamp = 32.0f;
            samplerDescriptor.maxAnisotropy = 1;
            post.sampler.Reset(wgpuDeviceCreateSampler(state.device.Get(), &samplerDescriptor));
            if (!post.sampler)
            {
                return fail("could not create the post-processing sampler");
            }

            const auto makePipeline = [&](const bool withDepth, RenderPipelineRef& pipeline)
            {
                WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
                depthStencil.format = GetDepthFormat();
                depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
                depthStencil.depthCompare = WGPUCompareFunction_Always;
                WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
                target.format = GetColorFormat();
                target.blend = nullptr;
                target.writeMask = WGPUColorWriteMask_All;
                WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
                fragment.module = post.module.Get();
                fragment.entryPoint = ToView("fs_main");
                fragment.targetCount = 1;
                fragment.targets = &target;
                WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                descriptor.label = ToView(withDepth ? "N2Engine post-processing (frame)" : "N2Engine post-processing");
                descriptor.layout = post.pipelineLayout.Get();
                descriptor.vertex.module = post.module.Get();
                descriptor.vertex.entryPoint = ToView("vs_main");
                descriptor.vertex.bufferCount = 0;
                descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                descriptor.primitive.frontFace = WGPUFrontFace_CCW;
                descriptor.primitive.cullMode = WGPUCullMode_None;
                descriptor.depthStencil = withDepth ? &depthStencil : nullptr;
                descriptor.fragment = &fragment;
                pipeline.Reset(wgpuDeviceCreateRenderPipeline(state.device.Get(), &descriptor));
                return static_cast<bool>(pipeline);
            };
            if (!makePipeline(false, post.offscreenPipeline) || !makePipeline(true, post.framePipeline))
            {
                post.offscreenPipeline.Reset();
                post.framePipeline.Reset();
                return fail("could not create the post-processing pipelines");
            }
        }

        if (post.scene && post.width == _targetWidth && post.height == _targetHeight)
        {
            return true;
        }
        // Released only: commands already encoded keep the old textures alive
        post.sceneView.Reset();
        post.scene.Reset();
        post.bloomAView.Reset();
        post.bloomA.Reset();
        post.bloomBView.Reset();
        post.bloomB.Reset();
        post.width = 0;
        post.height = 0;
        const std::uint32_t bloomWidth = Common::BloomExtent(_targetWidth);
        const std::uint32_t bloomHeight = Common::BloomExtent(_targetHeight);
        if (!MakePostTexture(state, _targetWidth, _targetHeight, WGPUTextureUsage_CopyDst, "N2Engine post-processing copy",
                             post.scene, post.sceneView) ||
            !MakePostTexture(state, bloomWidth, bloomHeight, WGPUTextureUsage_RenderAttachment, "N2Engine bloom A",
                             post.bloomA, post.bloomAView) ||
            !MakePostTexture(state, bloomWidth, bloomHeight, WGPUTextureUsage_RenderAttachment, "N2Engine bloom B",
                             post.bloomB, post.bloomBView))
        {
            post.sceneView.Reset();
            post.scene.Reset();
            LogOnce("could not create the post-processing textures; post-processing is skipped");
            return false;
        }
        post.width = _targetWidth;
        post.height = _targetHeight;
        return true;
    }

    bool WebGpuRenderer::PostPassLocked(const WGPUTextureView target, const bool framePass, const WGPUTextureView source,
                                        const WGPUTextureView bloom, const PostProcessParams& params)
    {
        State& state = *_state;
        PostResources& post = state.post;
        std::size_t chunkIndex = 0;
        std::uint64_t offset = 0;
        if (!AllocateRing(AlignUp64(sizeof(PostProcessParams), state.uniformAlignment), chunkIndex, offset))
        {
            return false;
        }
        RingChunk& chunk = state.chunks[chunkIndex];
        std::memcpy(chunk.staging.data() + offset, &params, sizeof(PostProcessParams));

        std::array<WGPUBindGroupEntry, 4> entries;
        for (WGPUBindGroupEntry& entry : entries)
        {
            entry = WGPU_BIND_GROUP_ENTRY_INIT;
        }
        entries[0].binding = 0;
        entries[0].buffer = chunk.buffer.Get();
        entries[0].offset = offset;
        entries[0].size = sizeof(PostProcessParams);
        entries[1].binding = 1;
        entries[1].textureView = source;
        entries[2].binding = 2;
        entries[2].textureView = bloom;
        entries[3].binding = 3;
        entries[3].sampler = post.sampler.Get();
        WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine post-processing group");
        descriptor.layout = post.groupLayout.Get();
        descriptor.entryCount = entries.size();
        descriptor.entries = entries.data();
        const BindGroupRef group(wgpuDeviceCreateBindGroup(state.device.Get(), &descriptor));
        if (!group)
        {
            return false;
        }

        if (framePass)
        {
            const WGPURenderPassEncoder pass = state.pass.Get();
            if (pass == nullptr)
            {
                return false;
            }
            wgpuRenderPassEncoderSetPipeline(pass, post.framePipeline.Get());
            wgpuRenderPassEncoderSetBindGroup(pass, 0, group.Get(), 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            return true;
        }

        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = target;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {0.0, 0.0, 0.0, 0.0};
        WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        passDescriptor.label = ToView("N2Engine post-processing pass");
        passDescriptor.colorAttachmentCount = 1;
        passDescriptor.colorAttachments = &color;
        const RenderPassRef pass(wgpuCommandEncoderBeginRenderPass(state.encoder.Get(), &passDescriptor));
        if (!pass)
        {
            return false;
        }
        wgpuRenderPassEncoderSetPipeline(pass.Get(), post.offscreenPipeline.Get());
        wgpuRenderPassEncoderSetBindGroup(pass.Get(), 0, group.Get(), 0, nullptr);
        wgpuRenderPassEncoderDraw(pass.Get(), 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass.Get());
        return true;
    }

    void WebGpuRenderer::ApplyPostProcess(const PostProcessSettings& input)
    {
        const PostProcessSettings settings = PostProcessSettings::Sanitized(input);
        if (!settings.IsActive())
        {
            return;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_frameOpen || state.lost.load() || !state.encoder || !EnsurePostLocked())
        {
            return;
        }
        PostResources& post = state.post;

        const std::uint32_t bloomWidth = Common::BloomExtent(_targetWidth);
        const std::uint32_t bloomHeight = Common::BloomExtent(_targetHeight);
        // Ends the frame's pass and copies the frame into the scene texture
        const auto copyFrame = [&]
        {
            EndPass();
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = state.color.Get();
            WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            to.texture = post.scene.Get();
            const WGPUExtent3D extent = {_targetWidth, _targetHeight, 1};
            wgpuCommandEncoderCopyTextureToTexture(state.encoder.Get(), &from, &to, &extent);
        };
        // A half-size image from the copy (the first pass given), then `iterations` pairs of blur passes; the result is in
        // bloomA
        const auto halfPasses = [&](const PostProcessParams& first, const int iterations)
        {
            PostPassLocked(post.bloomAView.Get(), false, post.sceneView.Get(), post.sceneView.Get(), first);
            for (int pass = 0; pass < iterations; ++pass)
            {
                const float spread = static_cast<float>(pass + 1);
                PostProcessParams params = PostProcessParams::For(PostPassMode::Blur, settings);
                params.b[0] = spread / static_cast<float>(bloomWidth);
                params.b[1] = 0.0f;
                PostPassLocked(post.bloomBView.Get(), false, post.bloomAView.Get(), post.bloomAView.Get(), params);
                params.b[0] = 0.0f;
                params.b[1] = spread / static_cast<float>(bloomHeight);
                PostPassLocked(post.bloomAView.Get(), false, post.bloomBView.Get(), post.bloomBView.Get(), params);
            }
        };
        // Back into the frame's pass, and the pass that writes the frame
        const auto toFrame = [&](const WGPUTextureView second, const PostPassMode mode)
        {
            if (!BeginPass(true))
            {
                LogOnce("could not begin a render pass; the rest of the frame is skipped");
                return false;
            }
            PostPassLocked(state.colorView.Get(), true, post.sceneView.Get(), second,
                           PostProcessParams::For(mode, settings));
            return true;
        };

        if (settings.BlurActive())
        {
            copyFrame();
            halfPasses(PostProcessParams::For(PostPassMode::Copy, settings), settings.blurIterations);
            if (!toFrame(post.bloomAView.Get(), PostPassMode::Mix))
            {
                return;
            }
        }
        if (settings.CompositeActive())
        {
            copyFrame();
            const bool bloom = settings.BloomActive();
            if (bloom)
            {
                halfPasses(PostProcessParams::For(PostPassMode::Bright, settings), settings.bloomIterations);
            }
            static_cast<void>(toFrame(bloom ? post.bloomAView.Get() : post.sceneView.Get(), PostPassMode::Composite));
        }
    }

    Common::ITexture* WebGpuRenderer::CreateRenderTexture(const std::uint32_t width, const std::uint32_t height)
    {
        if (width == 0 || height == 0 || width > Common::MaxRenderTextureSize || height > Common::MaxRenderTextureSize)
        {
            return nullptr;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.device)
        {
            return nullptr;
        }
        const std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);
        std::unique_ptr<WebGpuTexture> texture =
            MakeTexture(state, pixels.data(), width, height, 4, Common::RenderTextureOptions(), true);
        if (!texture)
        {
            return nullptr;
        }
        Common::ITexture* raw = texture.get();
        {
            std::lock_guard idLock(state.textureIdMutex);
            texture->id = state.nextTextureId++;
            state.textureIds[raw] = texture->id;
        }
        state.textures.emplace(raw, std::move(texture));
        state.post.renderTextures.push_back(raw);
        return raw;
    }

    bool WebGpuRenderer::CaptureFrame(Common::ITexture* target)
    {
        if (target == nullptr)
        {
            return false;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_frameOpen || state.lost.load() || !state.encoder ||
            std::ranges::find(state.post.renderTextures, target) == state.post.renderTextures.end() || !EnsurePostLocked())
        {
            return false;
        }
        const auto it = state.textures.find(target);
        if (it == state.textures.end())
        {
            return false;
        }
        EndPass();
        PostProcessParams params = PostProcessParams::For(PostPassMode::Copy, PostProcessSettings{});
        params.c[3] = 1.0f; // the texture's first row is the bottom of the frame
        const bool drawn =
            PostPassLocked(it->second->view.Get(), false, state.colorView.Get(), state.colorView.Get(), params);
        if (!BeginPass(true))
        {
            LogOnce("could not begin a render pass; the rest of the frame is skipped");
            return false;
        }
        return drawn;
    }
}
