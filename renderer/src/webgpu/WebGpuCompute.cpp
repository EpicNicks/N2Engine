#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"

#include "WebGpuInternal.hpp"

// Common::IGpuCompute on the WebGPU renderer's device: storage and uniform buffers, WGSL compute kernels with one bind
// group, dispatches recorded into one command encoder and submitted together, and asynchronous reads (a copy into a
// MapRead staging buffer, then a map whose callback may run whenever the device delivers it). Nothing here waits, except
// WaitIdleForTest natively, so the same code runs in the browser build (emdawnwebgpu), where a map completes only after
// control returns to the browser's event loop.
//
// Every call takes the renderer's mutex. The objects live in WebGpuRenderer::State (compute*) and die with Shutdown, which
// calls DestroyComputeResourcesLocked; their ids come from a counter that never repeats, so an id from before names nothing.

namespace Renderer::WebGpu
{
    namespace
    {
#ifndef __EMSCRIPTEN__
        constexpr std::uint64_t WaitTimeoutNanoseconds = 10ull * 1000ull * 1000ull * 1000ull;

        struct QueueDone
        {
            bool done = false;
        };

        void OnComputeQueueDone(WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<QueueDone>> holder(static_cast<std::shared_ptr<QueueDone>*>(userdata1));
            (*holder)->done = true;
        }
#endif

        void OnComputeMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<MapResult>> holder(static_cast<std::shared_ptr<MapResult>*>(userdata1));
            MapResult& result = **holder;
            result.status = status;
            result.message = ToString(message);
            result.done = true;
        }

        WGPUBufferBindingType ToBindingType(const Common::GpuBindingKind kind)
        {
            switch (kind)
            {
            case Common::GpuBindingKind::Uniform:
                return WGPUBufferBindingType_Uniform;
            case Common::GpuBindingKind::ReadOnlyStorage:
                return WGPUBufferBindingType_ReadOnlyStorage;
            case Common::GpuBindingKind::Storage:
                break;
            }
            return WGPUBufferBindingType_Storage;
        }

        /// The kernel cache's key: the binding kinds, the entry point and the module text, so two kernels share a pipeline only
        /// when all three are equal
        std::string KernelKey(const std::string_view wgsl, const std::string_view entryPoint,
                              const std::span<const Common::GpuBindingKind> bindings)
        {
            std::string key;
            key.reserve(bindings.size() + entryPoint.size() + wgsl.size() + 2);
            for (const Common::GpuBindingKind kind : bindings)
            {
                key.push_back(static_cast<char>('0' + static_cast<int>(kind)));
            }
            key.push_back('|');
            key.append(entryPoint);
            key.push_back('|');
            key.append(wgsl);
            return key;
        }
    }

    /// What GetCompute returns: forwards every call to the renderer under its mutex
    class WebGpuRenderer::ComputeFacade final : public Common::IGpuCompute
    {
    public:
        explicit ComputeFacade(WebGpuRenderer& renderer) : _renderer(renderer) {}

        [[nodiscard]] bool IsUsable() const override
        {
            std::lock_guard lock(_renderer._mutex);
            return UsableLocked();
        }

        [[nodiscard]] std::string_view GetName() const override { return "WebGPU"; }

        [[nodiscard]] bool IsSoftwareDevice() const override
        {
            std::lock_guard lock(_renderer._mutex);
            return GetState().adapterType == WGPUAdapterType_CPU;
        }

        [[nodiscard]] std::uint64_t GetMaxBufferBytes() const override
        {
            std::lock_guard lock(_renderer._mutex);
            return UsableLocked() ? GetState().maxStorageBindingSize : 0;
        }

        [[nodiscard]] Common::GpuBufferId CreateBuffer(const Common::GpuBufferUsage usage, const std::uint64_t bytes) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked() || bytes == 0 || bytes % 4 != 0 || bytes > state.maxStorageBindingSize ||
                bytes > state.maxBufferSize)
            {
                return 0;
            }
            WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
            descriptor.label = ToView(usage == Common::GpuBufferUsage::Uniform ? "N2Engine compute uniform"
                                                                                : "N2Engine compute storage");
            descriptor.usage = usage == Common::GpuBufferUsage::Uniform
                                   ? (WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst)
                                   : (WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
            descriptor.size = bytes;
            // WebGPU zero-initialises every buffer it creates, so a new buffer reads all zero as promised
            BufferRef buffer(wgpuDeviceCreateBuffer(state.device.Get(), &descriptor));
            if (!buffer)
            {
                _renderer.LogOnce("WebGPU compute: could not create a buffer of " + std::to_string(bytes) + " bytes");
                return 0;
            }
            const std::uint64_t id = state.nextComputeId++;
            ComputeBuffer& entry = state.computeBuffers[id];
            entry.buffer = std::move(buffer);
            entry.size = bytes;
            entry.usage = usage;
            return id;
        }

        void DestroyBuffer(const Common::GpuBufferId buffer) override
        {
            std::lock_guard lock(_renderer._mutex);
            // Only released: WebGPU keeps the buffer alive for recorded and submitted work that uses it
            GetState().computeBuffers.erase(buffer);
        }

        bool WriteBuffer(const Common::GpuBufferId buffer, const std::uint64_t offset, const void* data,
                         const std::uint64_t bytes) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked() || data == nullptr)
            {
                return false;
            }
            const auto it = state.computeBuffers.find(buffer);
            if (it == state.computeBuffers.end() || offset % 4 != 0 || bytes % 4 != 0 || offset > it->second.size ||
                bytes > it->second.size - offset)
            {
                return false;
            }
            if (bytes == 0)
            {
                return true;
            }
            // A queue write runs on the queue's timeline at once: the dispatches recorded so far go first, so they see the
            // buffer as it was when they were recorded
            if (!SubmitLocked())
            {
                return false;
            }
            wgpuQueueWriteBuffer(state.queue.Get(), it->second.buffer.Get(), offset, data, static_cast<std::size_t>(bytes));
            return true;
        }

        [[nodiscard]] Common::GpuKernelId CreateKernel(const std::string_view label, const std::string_view wgsl,
                                                       const std::string_view entryPoint,
                                                       const std::span<const Common::GpuBindingKind> bindings) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked() || wgsl.empty() || entryPoint.empty())
            {
                return 0;
            }
            std::uint32_t storageCount = 0;
            for (const Common::GpuBindingKind kind : bindings)
            {
                storageCount += kind == Common::GpuBindingKind::Uniform ? 0u : 1u;
            }
            if (storageCount > state.maxStorageBuffersPerStage)
            {
                _renderer.LogOnce("WebGPU compute: kernel '" + std::string(label) + "' has " + std::to_string(storageCount) +
                                  " storage buffers, more than the device's " +
                                  std::to_string(state.maxStorageBuffersPerStage));
                return 0;
            }

            const std::string key = KernelKey(wgsl, entryPoint, bindings);
            if (const auto cached = state.computeKernelCache.find(key); cached != state.computeKernelCache.end())
            {
                if (state.computeKernels.contains(cached->second))
                {
                    return cached->second;
                }
                state.computeKernelCache.erase(cached);
            }

            const std::string labelText(label);
            WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
            source.code = WGPUStringView{wgsl.data(), wgsl.size()};
            WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            moduleDescriptor.label = ToView(labelText.c_str());
            moduleDescriptor.nextInChain = &source.chain;
            ShaderModuleRef module(wgpuDeviceCreateShaderModule(state.device.Get(), &moduleDescriptor));
            if (!module)
            {
                _renderer.LogOnce("WebGPU compute: could not create the shader module of '" + labelText + "'");
                return 0;
            }

            std::vector<WGPUBindGroupLayoutEntry> entries(bindings.size());
            for (std::size_t i = 0; i < bindings.size(); ++i)
            {
                entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                entries[i].binding = static_cast<std::uint32_t>(i);
                entries[i].visibility = WGPUShaderStage_Compute;
                entries[i].buffer.type = ToBindingType(bindings[i]);
                entries[i].buffer.hasDynamicOffset = WGPU_FALSE;
            }
            WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            layoutDescriptor.label = ToView(labelText.c_str());
            layoutDescriptor.entryCount = entries.size();
            layoutDescriptor.entries = entries.data();
            BindGroupLayoutRef layout(wgpuDeviceCreateBindGroupLayout(state.device.Get(), &layoutDescriptor));
            if (!layout)
            {
                _renderer.LogOnce("WebGPU compute: could not create the bind group layout of '" + labelText + "'");
                return 0;
            }
            const WGPUBindGroupLayout layoutHandle = layout.Get();
            WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pipelineLayoutDescriptor.label = ToView(labelText.c_str());
            pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
            pipelineLayoutDescriptor.bindGroupLayouts = &layoutHandle;
            PipelineLayoutRef pipelineLayout(wgpuDeviceCreatePipelineLayout(state.device.Get(), &pipelineLayoutDescriptor));
            if (!pipelineLayout)
            {
                _renderer.LogOnce("WebGPU compute: could not create the pipeline layout of '" + labelText + "'");
                return 0;
            }

            const std::string entryText(entryPoint);
            WGPUComputePipelineDescriptor pipelineDescriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            pipelineDescriptor.label = ToView(labelText.c_str());
            pipelineDescriptor.layout = pipelineLayout.Get();
            pipelineDescriptor.compute.module = module.Get();
            pipelineDescriptor.compute.entryPoint = ToView(entryText.c_str());
            // An invalid module or entry point still gives a (poisoned) pipeline; the device reports the error to the
            // renderer's uncaptured-error callback, and dispatches with it fail validation the same way
            ComputePipelineRef pipeline(wgpuDeviceCreateComputePipeline(state.device.Get(), &pipelineDescriptor));
            if (!pipeline)
            {
                _renderer.LogOnce("WebGPU compute: could not create the pipeline of '" + labelText + "'");
                return 0;
            }

            const std::uint64_t id = state.nextComputeId++;
            ComputeKernel& kernel = state.computeKernels[id];
            kernel.pipeline = std::move(pipeline);
            kernel.layout = std::move(layout);
            kernel.bindings.assign(bindings.begin(), bindings.end());
            state.computeKernelCache[key] = id;
            return id;
        }

        bool Dispatch(const Common::GpuKernelId kernelId, const std::span<const Common::GpuBufferId> buffers,
                      const std::uint32_t groupsX, const std::uint32_t groupsY, const std::uint32_t groupsZ) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked())
            {
                return false;
            }
            const auto kernelIt = state.computeKernels.find(kernelId);
            if (kernelIt == state.computeKernels.end() || buffers.size() != kernelIt->second.bindings.size())
            {
                return false;
            }
            const std::uint32_t limit = state.maxWorkgroupsPerDimension;
            if (groupsX > limit || groupsY > limit || groupsZ > limit)
            {
                return false;
            }
            const ComputeKernel& kernel = kernelIt->second;

            std::vector<WGPUBindGroupEntry> entries(buffers.size());
            for (std::size_t i = 0; i < buffers.size(); ++i)
            {
                const auto bufferIt = state.computeBuffers.find(buffers[i]);
                if (bufferIt == state.computeBuffers.end())
                {
                    return false;
                }
                const bool wantsUniform = kernel.bindings[i] == Common::GpuBindingKind::Uniform;
                if (wantsUniform != (bufferIt->second.usage == Common::GpuBufferUsage::Uniform))
                {
                    return false; // bound as the wrong kind of buffer: the device would refuse the bind group
                }
                entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
                entries[i].binding = static_cast<std::uint32_t>(i);
                entries[i].buffer = bufferIt->second.buffer.Get();
                entries[i].offset = 0;
                entries[i].size = bufferIt->second.size;
            }
            if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
            {
                return true;
            }

            WGPUBindGroupDescriptor groupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            groupDescriptor.label = ToView("N2Engine compute group");
            groupDescriptor.layout = kernel.layout.Get();
            groupDescriptor.entryCount = entries.size();
            groupDescriptor.entries = entries.data();
            BindGroupRef group(wgpuDeviceCreateBindGroup(state.device.Get(), &groupDescriptor));
            if (!group)
            {
                return false;
            }

            if (!state.computeEncoder)
            {
                state.computeEncoder.Reset(wgpuDeviceCreateCommandEncoder(state.device.Get(), nullptr));
                if (!state.computeEncoder)
                {
                    _renderer.LogOnce("WebGPU compute: could not create a command encoder");
                    return false;
                }
            }
            // One pass per dispatch: a pass is cheap, and storage written by one dispatch is visible to the next either way
            WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            ComputePassRef pass(wgpuCommandEncoderBeginComputePass(state.computeEncoder.Get(), &passDescriptor));
            if (!pass)
            {
                return false;
            }
            wgpuComputePassEncoderSetPipeline(pass.Get(), kernel.pipeline.Get());
            wgpuComputePassEncoderSetBindGroup(pass.Get(), 0, group.Get(), 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(pass.Get(), groupsX, groupsY, groupsZ);
            wgpuComputePassEncoderEnd(pass.Get());
            return true;
        }

        bool Submit() override
        {
            std::lock_guard lock(_renderer._mutex);
            if (!UsableLocked())
            {
                GetState().computeEncoder.Reset();
                return false;
            }
            return SubmitLocked();
        }

        [[nodiscard]] Common::GpuReadTicket RequestRead(const Common::GpuBufferId buffer, const std::uint64_t offset,
                                                        const std::uint64_t bytes) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked())
            {
                return 0;
            }
            const auto it = state.computeBuffers.find(buffer);
            if (it == state.computeBuffers.end() || it->second.usage != Common::GpuBufferUsage::Storage || bytes == 0 ||
                offset % 4 != 0 || bytes % 4 != 0 || offset > it->second.size || bytes > it->second.size - offset)
            {
                return 0;
            }

            WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine compute readback");
            descriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
            descriptor.size = bytes;
            BufferRef staging(wgpuDeviceCreateBuffer(state.device.Get(), &descriptor));
            if (!staging)
            {
                _renderer.LogOnce("WebGPU compute: could not create a readback buffer");
                return 0;
            }
            // The copy goes after the recorded dispatches, in the same submission
            if (!state.computeEncoder)
            {
                state.computeEncoder.Reset(wgpuDeviceCreateCommandEncoder(state.device.Get(), nullptr));
                if (!state.computeEncoder)
                {
                    return 0;
                }
            }
            wgpuCommandEncoderCopyBufferToBuffer(state.computeEncoder.Get(), it->second.buffer.Get(), offset, staging.Get(), 0,
                                                 bytes);
            if (!SubmitLocked())
            {
                return 0;
            }

            auto result = std::make_shared<MapResult>();
            WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = OnComputeMapped;
            callback.userdata1 = new std::shared_ptr<MapResult>(result);
            wgpuBufferMapAsync(staging.Get(), WGPUMapMode_Read, 0, static_cast<std::size_t>(bytes), callback);

            const std::uint64_t ticket = state.nextComputeId++;
            ComputeRead& read = state.computeReads[ticket];
            read.staging = std::move(staging);
            read.map = std::move(result);
            read.size = bytes;
            return ticket;
        }

        Common::GpuReadStatus PollRead(const Common::GpuReadTicket ticket, std::vector<std::uint8_t>& out) override
        {
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            const auto it = state.computeReads.find(ticket);
            if (it == state.computeReads.end())
            {
                return Common::GpuReadStatus::Failed;
            }
            if (!it->second.map->done && state.instance)
            {
                // Natively a map callback is delivered from here (it may also come spontaneously); in the browser the event
                // loop delivers it and this only processes what is already there
                wgpuInstanceProcessEvents(state.instance.Get());
            }
            ComputeRead& read = it->second;
            if (!read.map->done)
            {
                if (!UsableLocked())
                {
                    state.computeReads.erase(it);
                    return Common::GpuReadStatus::Failed;
                }
                return Common::GpuReadStatus::Pending;
            }
            Common::GpuReadStatus status = Common::GpuReadStatus::Failed;
            if (read.map->status == WGPUMapAsyncStatus_Success)
            {
                const auto* mapped = static_cast<const std::uint8_t*>(
                    wgpuBufferGetConstMappedRange(read.staging.Get(), 0, static_cast<std::size_t>(read.size)));
                if (mapped != nullptr)
                {
                    out.resize(static_cast<std::size_t>(read.size));
                    std::memcpy(out.data(), mapped, static_cast<std::size_t>(read.size));
                    status = Common::GpuReadStatus::Done;
                }
                wgpuBufferUnmap(read.staging.Get());
            }
            else
            {
                _renderer.LogOnce("WebGPU compute: a readback map failed: " + read.map->message);
            }
            state.computeReads.erase(it);
            return status;
        }

        void CancelRead(const Common::GpuReadTicket ticket) override
        {
            std::lock_guard lock(_renderer._mutex);
            // Releasing a buffer whose map is pending ends the map (its callback, if it still runs, only writes the result
            // block it holds a reference to)
            GetState().computeReads.erase(ticket);
        }

        bool WaitIdleForTest() override
        {
#ifdef __EMSCRIPTEN__
            return false;
#else
            std::lock_guard lock(_renderer._mutex);
            WebGpuRenderer::State& state = GetState();
            if (!UsableLocked() || !SubmitLocked())
            {
                return false;
            }
            auto result = std::make_shared<QueueDone>();
            WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_WaitAnyOnly;
            callback.callback = OnComputeQueueDone;
            callback.userdata1 = new std::shared_ptr<QueueDone>(result);
            const WGPUFuture future = wgpuQueueOnSubmittedWorkDone(state.queue.Get(), callback);
            WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
            info.future = future;
            if (wgpuInstanceWaitAny(state.instance.Get(), 1, &info, WaitTimeoutNanoseconds) != WGPUWaitStatus_Success ||
                !result->done)
            {
                return false;
            }
            // The maps of the reads in flight complete once the work before them has: deliver their callbacks (up to about
            // five seconds)
            for (int round = 0; round < 5000; ++round)
            {
                wgpuInstanceProcessEvents(state.instance.Get());
                const bool pending = std::ranges::any_of(state.computeReads, [](const auto& entry)
                                                         { return !entry.second.map->done; });
                if (!pending)
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
#endif
        }

    private:
        [[nodiscard]] WebGpuRenderer::State& GetState() const { return *_renderer._state; }

        /// With the mutex held
        [[nodiscard]] bool UsableLocked() const
        {
            const WebGpuRenderer::State& state = *_renderer._state;
            return _renderer._initialized && state.device && state.queue && !state.lost.load();
        }

        /// With the mutex held: submits the recorded dispatches and copies, if any. True when there was nothing to submit
        bool SubmitLocked() const
        {
            WebGpuRenderer::State& state = *_renderer._state;
            if (!state.computeEncoder)
            {
                return true;
            }
            CommandBufferRef commands(wgpuCommandEncoderFinish(state.computeEncoder.Get(), nullptr));
            state.computeEncoder.Reset();
            if (!commands)
            {
                _renderer.LogOnce("WebGPU compute: could not finish the recorded commands");
                return false;
            }
            const WGPUCommandBuffer handle = commands.Get();
            wgpuQueueSubmit(state.queue.Get(), 1, &handle);
            return true;
        }

        WebGpuRenderer& _renderer;
    };

    Common::IGpuCompute* WebGpuRenderer::GetCompute()
    {
        std::lock_guard lock(_mutex);
        if (!_initialized || !_state->device || _state->lost.load())
        {
            return nullptr;
        }
        if (!_compute)
        {
            _compute = std::make_shared<ComputeFacade>(*this);
        }
        return _compute.get();
    }

    void WebGpuRenderer::DestroyComputeResourcesLocked()
    {
        State& state = *_state;
        state.computeEncoder.Reset();
        state.computeReads.clear();
        state.computeKernelCache.clear();
        state.computeKernels.clear();
        state.computeBuffers.clear();
    }
}
