#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"

#include "WebGpuInternal.hpp"

// Stage G2 of the WebGPU backend (#155): headless initialisation, an offscreen colour and depth target, a frame that clears
// it, a synchronous readback, resizing, shutdown and the counting of every error the device reports. Meshes, textures and
// the samplers are in WebGpuResources.cpp and the shaders, materials, pipelines, uniform ring and draws in WebGpuDraw.cpp
// (stages G3a and G3b).
//
// Written against webgpu.h (the C API) only, so that stage G4 can compile this file with Emscripten's emdawnwebgpu port. What
// exists only natively is marked "NATIVE ONLY": asking for the Vulkan backend by type, and the blocking waits
// (wgpuInstanceWaitAny with a timeout needs the TimedWaitAny instance feature, which a browser does not have: there a
// readback cannot block at all, see the design in the proposal's section 4.3).
//
// Object lifetimes, in creation order (ShutdownLocked releases them in reverse):
//   instance -> adapter -> device (+ its queue) -> colour and depth textures with a view each -> the draw resources (shader
//   module, layouts, default textures: WebGpuDraw.cpp) -> meshes, textures, materials, pipelines and ring chunks as they are
//   made -> per frame: a command encoder and a render pass encoder, then a command buffer
// Every handle is held by a small reference-counting wrapper (Ref) that also counts the live references of the process
// (GetLiveObjectCountForTest), which is how the tests see a leak.
//
// Errors: the device is created with an uncaptured-error callback and a device-lost callback, both counting into one block of
// counters (ErrorCounters) that the renderer keeps for its whole life. Dawn delivers errors asynchronously, so
// GetValidationErrorCount first flushes (processes events, waits for the queue).

namespace Renderer::WebGpu
{
    namespace
    {
        constexpr std::size_t MaxStoredMessages = 32;
        constexpr std::uint64_t MapTimeoutNanoseconds = 10ull * 1000ull * 1000ull * 1000ull;
        constexpr std::uint64_t RequestTimeoutNanoseconds = 30ull * 1000ull * 1000ull * 1000ull;
        constexpr std::uint64_t FlushTimeoutNanoseconds = 5ull * 1000ull * 1000ull * 1000ull;
        // The core defaults, used only if the device cannot say (wgpuDeviceGetLimits failing)
        constexpr std::uint32_t DefaultMaxTextureDimension = 8192;
        constexpr std::uint64_t DefaultMaxBufferSize = 268435456;
        constexpr std::uint32_t DefaultUniformAlignment = 256;

        std::string GetEnvironment(const char* name)
        {
            std::string value;
#ifdef _MSC_VER
            char* buffer = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr)
            {
                value = buffer;
                std::free(buffer);
            }
#else
            if (const char* text = std::getenv(name); text != nullptr)
            {
                value = text;
            }
#endif
            return value;
        }

        const char* ErrorTypeName(WGPUErrorType type)
        {
            switch (type)
            {
            case WGPUErrorType_Validation:
                return "validation error";
            case WGPUErrorType_OutOfMemory:
                return "out of memory";
            case WGPUErrorType_Internal:
                return "internal error";
            default:
                return "unknown error";
            }
        }

        const char* LostReasonName(WGPUDeviceLostReason reason)
        {
            switch (reason)
            {
            case WGPUDeviceLostReason_Destroyed:
                return "destroyed";
            case WGPUDeviceLostReason_CallbackCancelled:
                return "callback cancelled";
            case WGPUDeviceLostReason_FailedCreation:
                return "failed creation";
            default:
                return "unknown";
            }
        }

        void OnUncapturedError(const WGPUDevice*, WGPUErrorType type, WGPUStringView message, void* userdata1, void*)
        {
            auto* counters = static_cast<ErrorCounters*>(userdata1);
            const std::string text = std::string(ErrorTypeName(type)) + ": " + ToString(message);
            std::cerr << "WebGPU renderer: " << text << std::endl;
            std::lock_guard lock(counters->mutex);
            ++counters->errors;
            if (counters->messages.size() < MaxStoredMessages)
            {
                counters->messages.push_back(text);
            }
        }

        void OnDeviceLost(const WGPUDevice*, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void*)
        {
            auto* counters = static_cast<ErrorCounters*>(userdata1);
            if (counters->expectedLoss.load())
            {
                return; // the renderer's own Shutdown
            }
            const std::string text = std::string("device lost (") + LostReasonName(reason) + "): " + ToString(message);
            std::cerr << "WebGPU renderer: " << text << std::endl;
            counters->lost = true;
            std::lock_guard lock(counters->mutex);
            ++counters->errors;
            ++counters->lostCount;
            if (counters->messages.size() < MaxStoredMessages)
            {
                counters->messages.push_back(text);
            }
        }

        // The results of the asynchronous calls. Each callback gets a heap copy of the shared pointer as its userdata, so
        // the result is still there for a callback that runs after the waiter gave up (the copy is freed by the callback)
        struct AdapterResult
        {
            bool done = false;
            WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Error;
            WGPUAdapter adapter = nullptr;
            std::string message;
        };
        struct DeviceResult
        {
            bool done = false;
            WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Error;
            WGPUDevice device = nullptr;
            std::string message;
        };
        struct MapResult
        {
            bool done = false;
            WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
            std::string message;
        };
        struct QueueResult
        {
            bool done = false;
            WGPUQueueWorkDoneStatus status = WGPUQueueWorkDoneStatus_Error;
        };

        template <typename Result>
        std::shared_ptr<Result>* NewUserdata(const std::shared_ptr<Result>& result)
        {
            return new std::shared_ptr<Result>(result);
        }

        void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1,
                       void*)
        {
            std::unique_ptr<std::shared_ptr<AdapterResult>> holder(static_cast<std::shared_ptr<AdapterResult>*>(userdata1));
            AdapterResult& result = **holder;
            result.status = status;
            result.adapter = adapter;
            result.message = ToString(message);
            result.done = true;
        }

        void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<DeviceResult>> holder(static_cast<std::shared_ptr<DeviceResult>*>(userdata1));
            DeviceResult& result = **holder;
            result.status = status;
            result.device = device;
            result.message = ToString(message);
            result.done = true;
        }

        void OnMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<MapResult>> holder(static_cast<std::shared_ptr<MapResult>*>(userdata1));
            MapResult& result = **holder;
            result.status = status;
            result.message = ToString(message);
            result.done = true;
        }

        void OnQueueDone(WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<QueueResult>> holder(static_cast<std::shared_ptr<QueueResult>*>(userdata1));
            QueueResult& result = **holder;
            result.status = status;
            result.done = true;
        }

        /// NATIVE ONLY (the timeout): waits for one future. True when it completed
        bool WaitFor(WGPUInstance instance, WGPUFuture future, std::uint64_t timeoutNanoseconds)
        {
            WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
            info.future = future;
            const WGPUWaitStatus status = wgpuInstanceWaitAny(instance, 1, &info, timeoutNanoseconds);
            return status == WGPUWaitStatus_Success;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Construction and state
    // -----------------------------------------------------------------------------------------------------------------

    WebGpuRenderer::WebGpuRenderer() : _state(std::make_shared<State>()) {}

    WebGpuRenderer::~WebGpuRenderer()
    {
        std::lock_guard lock(_mutex);
        ShutdownLocked();
    }

    void WebGpuRenderer::SetAdapterPreference(AdapterPreference preference)
    {
        std::lock_guard lock(_mutex);
        _preference = preference;
    }

    InitStatus WebGpuRenderer::GetInitStatus() const
    {
        std::lock_guard lock(_mutex);
        return _initStatus;
    }

    std::string WebGpuRenderer::GetInitMessage() const
    {
        std::lock_guard lock(_mutex);
        return _initMessage;
    }

    std::string WebGpuRenderer::GetAdapterName() const
    {
        std::lock_guard lock(_mutex);
        return _state->adapterName;
    }

    WGPUAdapterType WebGpuRenderer::GetAdapterType() const
    {
        std::lock_guard lock(_mutex);
        return _state->adapterType;
    }

    WGPUBackendType WebGpuRenderer::GetBackendType() const
    {
        std::lock_guard lock(_mutex);
        return _state->backendType;
    }

    std::uint32_t WebGpuRenderer::GetRenderWidth() const
    {
        std::lock_guard lock(_mutex);
        return _targetWidth;
    }

    std::uint32_t WebGpuRenderer::GetRenderHeight() const
    {
        std::lock_guard lock(_mutex);
        return _targetHeight;
    }

    std::uint32_t WebGpuRenderer::GetValidationErrorCount() const
    {
        {
            std::lock_guard lock(_mutex);
            FlushLocked();
        }
        std::lock_guard lock(_state->mutex);
        return _state->errors;
    }

    std::vector<std::string> WebGpuRenderer::GetValidationMessages() const
    {
        std::lock_guard lock(_state->mutex);
        return _state->messages;
    }

    std::uint32_t WebGpuRenderer::GetDeviceLostCount() const
    {
        std::lock_guard lock(_state->mutex);
        return _state->lostCount;
    }

    bool WebGpuRenderer::IsDeviceLost() const
    {
        return _state->lost.load();
    }

    void WebGpuRenderer::ResetValidationErrors()
    {
        {
            std::lock_guard lock(_mutex);
            FlushLocked(); // what is still in flight belongs to what the caller wants to forget
        }
        std::lock_guard lock(_state->mutex);
        _state->errors = 0;
        _state->lostCount = 0;
        _state->messages.clear();
    }

    WGPUInstance WebGpuRenderer::GetInstanceForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->instance.Get();
    }

    WGPUDevice WebGpuRenderer::GetDeviceForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->device.Get();
    }

    WGPUQueue WebGpuRenderer::GetQueueForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->queue.Get();
    }

    WGPUTexture WebGpuRenderer::GetColorTextureForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->color.Get();
    }

    WGPUTexture WebGpuRenderer::GetDepthTextureForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->depth.Get();
    }

    std::uint32_t WebGpuRenderer::GetMaxTextureDimensionForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->maxTextureDimension;
    }

    std::uint64_t WebGpuRenderer::GetMaxBufferSizeForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->maxBufferSize;
    }

    void WebGpuRenderer::SetMaxBufferSizeForTest(std::uint64_t bytes)
    {
        std::lock_guard lock(_mutex);
        _state->maxBufferSize = bytes;
    }

    std::int64_t WebGpuRenderer::GetLiveObjectCountForTest()
    {
        return g_liveObjects.load();
    }

    void WebGpuRenderer::LogOnce(const std::string& message) const
    {
        {
            std::lock_guard lock(_logMutex);
            if (!_logged.insert(message).second)
            {
                return;
            }
        }
        std::cerr << "WebGPU renderer: " << message << std::endl;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Initialisation
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::Fail(InitStatus status, const std::string& message)
    {
        _initStatus = status;
        _initMessage = message;
        std::cerr << "WebGPU renderer: " << message << std::endl;
        return false;
    }

    bool WebGpuRenderer::Initialize(GLFWwindow* windowHandle, std::uint32_t width, std::uint32_t height)
    {
        std::lock_guard lock(_mutex);

        if (_state->instance)
        {
            // Initialized again without a Shutdown: a new renderer for everything holding the old one's token
            ShutdownLocked();
            EndLifetime();
        }
        _initStatus = InitStatus::NotInitialized;
        _initMessage.clear();
        _warnedPresent = false;
        _targetFixed = false;
        static_cast<void>(windowHandle); // no surface: frames render offscreen whether or not there is a window

        if (width == 0 || height == 0)
        {
            return Fail(InitStatus::Failed, "Initialize needs a non-zero size");
        }

        std::uint32_t before = 0;
        {
            std::lock_guard counterLock(_state->mutex);
            before = _state->errors;
        }
        bool ok = CreateInstance() && PickAdapter() && CreateDevice() && CreateTargets(width, height) && CreateDrawResources();
        if (ok)
        {
            // Errors reach the callback asynchronously: have the device deliver what creating all this caused, then count
            FlushLocked();
            std::uint32_t after = 0;
            {
                std::lock_guard counterLock(_state->mutex);
                after = _state->errors;
            }
            if (after != before)
            {
                ok = Fail(InitStatus::Failed, "the device reported an error while it was being set up (see above)");
            }
        }
        if (!ok)
        {
            // Every step above leaves what it made in the state; release it all, keeping the reason
            const InitStatus status = _initStatus;
            const std::string message = _initMessage;
            ShutdownLocked();
            _initStatus = status == InitStatus::NotInitialized ? InitStatus::Failed : status;
            _initMessage = message;
            return false;
        }

        _initialized = true;
        _initStatus = InitStatus::Ok;
        return true;
    }

    bool WebGpuRenderer::CreateInstance()
    {
        // TimedWaitAny lets wgpuInstanceWaitAny block with a timeout: how the synchronous waits of this file work (NATIVE ONLY)
        const WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
        WGPUInstanceDescriptor descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
        descriptor.requiredFeatureCount = 1;
        descriptor.requiredFeatures = features;
        _state->instance.Reset(wgpuCreateInstance(&descriptor));
        if (!_state->instance)
        {
            return Fail(InitStatus::NoInstance, "wgpuCreateInstance returned null (Dawn could not create an instance)");
        }
        return true;
    }

    bool WebGpuRenderer::PickAdapter()
    {
        AdapterPreference preference = _preference;
        if (preference == AdapterPreference::Default)
        {
            preference = GetEnvironment("N2_WEBGPU_EXPECT_CPU_DEVICE") == "1" ? AdapterPreference::CpuOnly
                                                                              : AdapterPreference::Any;
        }

        const auto request = [this](bool fallbackOnly, std::string& message) -> WGPUAdapter
        {
            WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
            options.featureLevel = WGPUFeatureLevel_Core;
            options.powerPreference = WGPUPowerPreference_HighPerformance;
            options.forceFallbackAdapter = fallbackOnly ? WGPU_TRUE : WGPU_FALSE;
            // NATIVE ONLY: the CI harness runs on Dawn's Vulkan backend (lavapipe). A browser leaves this undefined
            options.backendType = WGPUBackendType_Vulkan;

            auto result = std::make_shared<AdapterResult>();
            WGPURequestAdapterCallbackInfo callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_WaitAnyOnly;
            callback.callback = OnAdapter;
            callback.userdata1 = NewUserdata(result);
            const WGPUFuture future = wgpuInstanceRequestAdapter(_state->instance.Get(), &options, callback);
            if (!WaitFor(_state->instance.Get(), future, RequestTimeoutNanoseconds) || !result->done)
            {
                message = "requesting an adapter did not finish";
                return nullptr;
            }
            message = result->message;
            return result->status == WGPURequestAdapterStatus_Success ? result->adapter : nullptr;
        };

        // Dawn's adapter request has no "take a CPU adapter" option other than forceFallbackAdapter, which not every software
        // driver answers to: ask for Dawn's preferred adapter first and then for the fallback one, and look at what came
        // back. CpuOnly keeps an adapter only if it says it is a CPU one (Mesa lavapipe, SwiftShader)
        std::string message;
        for (const bool fallbackOnly : {false, true})
        {
            std::string attemptMessage;
            WGPUAdapter adapter = request(fallbackOnly, attemptMessage);
            if (adapter == nullptr)
            {
                if (message.empty())
                {
                    message = attemptMessage;
                }
                continue;
            }
            _state->adapter.Reset(adapter);
            _state->adapterName.clear();
            _state->adapterType = WGPUAdapterType_Unknown;
            _state->backendType = WGPUBackendType_Undefined;
            WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
            if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success)
            {
                _state->adapterName = ToString(info.device);
                const std::string description = ToString(info.description);
                if (!description.empty())
                {
                    _state->adapterName += " (" + description + ")";
                }
                _state->adapterType = info.adapterType;
                _state->backendType = info.backendType;
                wgpuAdapterInfoFreeMembers(info);
            }
            if (preference == AdapterPreference::CpuOnly && _state->adapterType != WGPUAdapterType_CPU)
            {
                message = "the adapter is " + _state->adapterName + ", not a CPU adapter";
                _state->adapter.Reset();
                continue;
            }
            return true;
        }
        _state->adapterName.clear();
        return Fail(InitStatus::NoAdapter,
                    std::string("Dawn found no Vulkan ") + (preference == AdapterPreference::CpuOnly ? "CPU " : "") + "adapter" +
                        (message.empty() ? "" : ": " + message) +
                        (preference == AdapterPreference::CpuOnly
                             ? " (N2_WEBGPU_EXPECT_CPU_DEVICE=1 asks for a CPU adapter such as Mesa lavapipe)"
                             : ""));
    }

    bool WebGpuRenderer::CreateDevice()
    {
        // The two callbacks count every error and every loss the renderer did not cause (ErrorCounters). A loss while the
        // renderer destroys its own device is expected (Shutdown sets expectedLoss first)
        WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine WebGPU renderer");
        descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        descriptor.deviceLostCallbackInfo.callback = OnDeviceLost;
        descriptor.deviceLostCallbackInfo.userdata1 = static_cast<ErrorCounters*>(_state.get());
        descriptor.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
        descriptor.uncapturedErrorCallbackInfo.userdata1 = static_cast<ErrorCounters*>(_state.get());

        auto result = std::make_shared<DeviceResult>();
        WGPURequestDeviceCallbackInfo callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = OnDevice;
        callback.userdata1 = NewUserdata(result);
        const WGPUFuture future = wgpuAdapterRequestDevice(_state->adapter.Get(), &descriptor, callback);
        if (!WaitFor(_state->instance.Get(), future, RequestTimeoutNanoseconds) || !result->done)
        {
            return Fail(InitStatus::Failed, "requesting a device did not finish");
        }
        if (result->status != WGPURequestDeviceStatus_Success || result->device == nullptr)
        {
            return Fail(InitStatus::Failed, "could not create a device: " + result->message);
        }
        _state->device.Reset(result->device);
        _state->queue.Reset(wgpuDeviceGetQueue(_state->device.Get()));
        if (!_state->queue)
        {
            return Fail(InitStatus::Failed, "the device has no queue");
        }

        WGPULimits limits = WGPU_LIMITS_INIT;
        if (wgpuDeviceGetLimits(_state->device.Get(), &limits) == WGPUStatus_Success)
        {
            _state->maxTextureDimension = limits.maxTextureDimension2D;
            _state->maxBufferSize = limits.maxBufferSize;
            const bool alignmentKnown =
                limits.minUniformBufferOffsetAlignment != 0 && limits.minUniformBufferOffsetAlignment != WGPU_LIMIT_U32_UNDEFINED;
            _state->uniformAlignment = alignmentKnown ? limits.minUniformBufferOffsetAlignment : DefaultUniformAlignment;
            // What the draws need (WebGpuDraw.cpp): in group 0 a 192-byte per-draw block and a 752-byte lighting block, each bound
            // at a dynamic offset (stage G3b added the lighting block: it raised the uniform binding size and the dynamic buffer
            // count from 2 to 3 and the uniform buffers per stage to 3), in group 1 a material block at a dynamic offset and five
            // textures with five samplers. Every WebGPU device offers at least the core defaults (65536 bytes, 8 dynamic
            // buffers, 12 uniform buffers per stage, 16 sampled textures and samplers), which are more than this; the check says
            // so rather than assume it
            const std::uint32_t slots = ShaderInterface::kSamplerCount;
            const std::uint32_t largestBlock = std::max(ShaderInterface::kPerDrawBlockSize, ShaderInterface::kLightingBlockSize);
            if (limits.maxUniformBufferBindingSize < largestBlock || limits.maxDynamicUniformBuffersPerPipelineLayout < 3 ||
                limits.maxUniformBuffersPerShaderStage < 3 || limits.maxBindGroups < 2 ||
                limits.maxBindingsPerBindGroup < 1 + 2 * slots || limits.maxSampledTexturesPerShaderStage < slots ||
                limits.maxSamplersPerShaderStage < slots)
            {
                return Fail(InitStatus::Failed, "the device's limits are below what the draws need (a uniform binding of " +
                                                    std::to_string(largestBlock) +
                                                    " bytes, 3 dynamic uniform buffers and 3 uniform buffers per stage, 2 bind groups, " +
                                                    std::to_string(1 + 2 * slots) + " bindings and " + std::to_string(slots) +
                                                    " textures and samplers per stage)");
            }
        }
        else
        {
            _state->maxTextureDimension = DefaultMaxTextureDimension;
            _state->maxBufferSize = DefaultMaxBufferSize;
            _state->uniformAlignment = DefaultUniformAlignment;
        }
        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The offscreen target
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::CreateTargets(std::uint32_t width, std::uint32_t height)
    {
        State& state = *_state;
        if (width > state.maxTextureDimension || height > state.maxTextureDimension)
        {
            return Fail(InitStatus::Failed, "a render target of " + std::to_string(width) + "x" + std::to_string(height) +
                                                " is over the device's texture limit (" +
                                                std::to_string(state.maxTextureDimension) + ")");
        }
        // The readback copies the target into a buffer with rows padded to 256 bytes: it has to fit one
        const std::uint64_t readbackBytes = static_cast<std::uint64_t>(AlignUp(width * 4, RowAlignment)) * height;
        if (readbackBytes > state.maxBufferSize)
        {
            return Fail(InitStatus::Failed, "a render target of " + std::to_string(width) + "x" + std::to_string(height) +
                                                " does not fit the device's buffer limit for readback (" +
                                                std::to_string(state.maxBufferSize) + " bytes)");
        }

        WGPUTextureDescriptor colorDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        colorDescriptor.label = ToView("N2Engine colour target");
        // CopyDst as well: a test writes a pattern into the target to prove the readback's orientation
        colorDescriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
        colorDescriptor.dimension = WGPUTextureDimension_2D;
        colorDescriptor.size = {width, height, 1};
        colorDescriptor.format = GetColorFormat();
        state.color.Reset(wgpuDeviceCreateTexture(state.device.Get(), &colorDescriptor));

        WGPUTextureDescriptor depthDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        depthDescriptor.label = ToView("N2Engine depth target");
        depthDescriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        depthDescriptor.dimension = WGPUTextureDimension_2D;
        depthDescriptor.size = {width, height, 1};
        depthDescriptor.format = GetDepthFormat();
        state.depth.Reset(wgpuDeviceCreateTexture(state.device.Get(), &depthDescriptor));

        if (!state.color || !state.depth)
        {
            DestroyTargets();
            return Fail(InitStatus::Failed, "could not create the render target textures");
        }

        // The default views: the whole texture, one mip level (the descriptor's defaults)
        state.colorView.Reset(wgpuTextureCreateView(state.color.Get(), nullptr));
        state.depthView.Reset(wgpuTextureCreateView(state.depth.Get(), nullptr));
        if (!state.colorView || !state.depthView)
        {
            DestroyTargets();
            return Fail(InitStatus::Failed, "could not create the render target views");
        }
        _targetWidth = width;
        _targetHeight = height;
        return true;
    }

    void WebGpuRenderer::DestroyTargets()
    {
        State& state = *_state;
        state.depthView.Reset();
        state.colorView.Reset();
        if (state.depth)
        {
            wgpuTextureDestroy(state.depth.Get()); // frees the memory now rather than at the last release
        }
        state.depth.Reset();
        if (state.color)
        {
            wgpuTextureDestroy(state.color.Get());
        }
        state.color.Reset();
        _targetWidth = 0;
        _targetHeight = 0;
    }

    void WebGpuRenderer::AbandonOpenFrame()
    {
        if (!_frameOpen)
        {
            return;
        }
        // Recorded against the old targets and never submitted: end the pass (a pass that is released open is an error) and
        // drop the encoder without finishing it
        EndPass();
        _state->encoder.Reset();
        _frameOpen = false;
    }

    bool WebGpuRenderer::RecreateTargets(std::uint32_t width, std::uint32_t height)
    {
        AbandonOpenFrame();
        DestroyTargets();
        const InitStatus statusBefore = _initStatus;
        const std::string messageBefore = _initMessage;
        const bool ok = CreateTargets(width, height);
        // A failed resize is not an initialisation failure: it reports through the return value and the log only
        _initStatus = statusBefore;
        _initMessage = messageBefore;
        return ok;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------------------------------------------------

    void WebGpuRenderer::Shutdown()
    {
        std::lock_guard lock(_mutex);
        ShutdownLocked();
        _initStatus = InitStatus::NotInitialized;
        _initMessage.clear();
        EndLifetime();
    }

    void WebGpuRenderer::ShutdownLocked()
    {
        State& state = *_state;
        if (state.device)
        {
            AbandonOpenFrame();
            FlushLocked(); // what the last frames caused is counted before the teardown starts
        }
        // From here the device's loss is the renderer's own doing, however late its callback arrives
        state.expectedLoss = true;
        _frameOpen = false;
        state.pass.Reset();
        state.encoder.Reset();
        DestroyAllResources(); // meshes, textures, materials, pipelines, the ring: all released before the device is
        DestroyTargets();
        state.queue.Reset();
        if (state.device)
        {
            wgpuDeviceDestroy(state.device.Get()); // the loss callback (reason Destroyed) is expected now
            if (state.instance)
            {
                wgpuInstanceProcessEvents(state.instance.Get()); // let it be delivered while the instance exists
            }
        }
        state.device.Reset();
        state.adapter.Reset();
        state.instance.Reset();

        state.adapterName.clear();
        state.adapterType = WGPUAdapterType_Unknown;
        state.backendType = WGPUBackendType_Undefined;
        state.maxTextureDimension = 0;
        state.maxBufferSize = 0;
        state.uniformAlignment = DefaultUniformAlignment;
        state.lost = false;
        state.expectedLoss = false;
        _targetWidth = 0;
        _targetHeight = 0;
        _targetFixed = false;
        _initialized = false;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Frames
    // -----------------------------------------------------------------------------------------------------------------

    void WebGpuRenderer::FlushLocked() const
    {
        State& state = *_state;
        if (!state.instance)
        {
            return;
        }
        wgpuInstanceProcessEvents(state.instance.Get());
        if (!state.queue || state.lost.load())
        {
            return;
        }
        // Errors found while the queue runs its work (and errors of calls not yet processed) arrive once it is idle
        auto result = std::make_shared<QueueResult>();
        WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = OnQueueDone;
        callback.userdata1 = NewUserdata(result);
        const WGPUFuture future = wgpuQueueOnSubmittedWorkDone(state.queue.Get(), callback);
        WaitFor(state.instance.Get(), future, FlushTimeoutNanoseconds);
        wgpuInstanceProcessEvents(state.instance.Get());
    }

    bool WebGpuRenderer::BeginPass()
    {
        State& state = *_state;
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = state.colorView.Get();
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {static_cast<double>(_clearColor[0]), static_cast<double>(_clearColor[1]),
                            static_cast<double>(_clearColor[2]), static_cast<double>(_clearColor[3])};

        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depth.view = state.depthView.Get();
        depth.depthLoadOp = WGPULoadOp_Clear;
        depth.depthStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = 1.0f;

        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine frame");
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &color;
        descriptor.depthStencilAttachment = &depth;
        state.pass.Reset(wgpuCommandEncoderBeginRenderPass(state.encoder.Get(), &descriptor));
        return static_cast<bool>(state.pass);
    }

    void WebGpuRenderer::EndPass()
    {
        State& state = *_state;
        if (state.pass)
        {
            wgpuRenderPassEncoderEnd(state.pass.Get());
            state.pass.Reset();
        }
    }

    void WebGpuRenderer::Clear(float r, float g, float b, float a)
    {
        std::lock_guard lock(_mutex);
        _clearColor[0] = r;
        _clearColor[1] = g;
        _clearColor[2] = b;
        _clearColor[3] = a;

        if (_frameOpen)
        {
            // Like glClear in the middle of a frame: clears what has been drawn so far, colour and depth. A render pass
            // cannot clear part-way, so the pass ends and a new one begins, clearing: whatever the first one drew is
            // overwritten
            EndPass();
            if (!BeginPass())
            {
                LogOnce("could not begin a render pass; the rest of the frame is skipped");
            }
        }
    }

    void WebGpuRenderer::BeginFrame()
    {
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.colorView)
        {
            return;
        }
        if (_frameOpen)
        {
            LogOnce("BeginFrame while a frame is already open (ignored)");
            return;
        }
        state.encoder.Reset(wgpuDeviceCreateCommandEncoder(state.device.Get(), nullptr));
        if (!state.encoder || !BeginPass())
        {
            state.encoder.Reset();
            LogOnce("could not begin a frame; frames are skipped");
            return;
        }
        ++state.frameSerial; // the frame being recorded: the in-frame UpdateMesh rule compares against it
        BeginDrawFrame();
        _frameOpen = true;
    }

    void WebGpuRenderer::EndFrame()
    {
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_frameOpen)
        {
            return;
        }
        _frameOpen = false;
        EndPass();
        CommandBufferRef commands(wgpuCommandEncoderFinish(state.encoder.Get(), nullptr));
        state.encoder.Reset();
        // The draws' uniform data goes to the ring buffers now, before the submit: a queue write runs on the queue's timeline
        // in call order, so the commands submitted next see it and the frames submitted before it do not
        EndDrawFrame();
        if (!commands)
        {
            LogOnce("could not finish a frame's commands; the frame is skipped");
            return;
        }
        const WGPUCommandBuffer handle = commands.Get();
        wgpuQueueSubmit(state.queue.Get(), 1, &handle);
        // `commands` is released here: the queue holds the work it needs
    }

    void WebGpuRenderer::Present()
    {
        // No swapchain or window (the browser's canvas arrives in stage G5): a frame is only ever read back. Say so once
        // per renderer, because with a window the user would otherwise just see it stay blank
        {
            std::lock_guard lock(_mutex);
            if (_warnedPresent)
            {
                return;
            }
            _warnedPresent = true;
        }
        std::cerr << "WebGPU renderer: Present has nothing to present (no swapchain or window yet): frames render offscreen "
                     "and are only readable through ReadFramebuffer"
                  << std::endl;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Readback
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::CopyTextureToHostLocked(WGPUTexture texture, WGPUTextureAspect aspect, std::uint32_t bytesPerPixel,
                                                 std::uint32_t width, std::uint32_t height,
                                                 std::vector<std::uint8_t>& pixels, std::uint32_t mipLevel) const
    {
        State& state = *_state;
        const std::uint32_t rowBytes = width * bytesPerPixel;
        const std::uint32_t paddedRowBytes = AlignUp(rowBytes, RowAlignment);
        const std::uint64_t size = static_cast<std::uint64_t>(paddedRowBytes) * height;
        if (size > state.maxBufferSize)
        {
            std::cerr << "WebGPU renderer: the readback buffer would be over the device's buffer limit" << std::endl;
            return false;
        }

        WGPUBufferDescriptor bufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        bufferDescriptor.label = ToView("N2Engine readback");
        bufferDescriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bufferDescriptor.size = size;
        BufferRef buffer(wgpuDeviceCreateBuffer(state.device.Get(), &bufferDescriptor));
        if (!buffer)
        {
            std::cerr << "WebGPU renderer: could not create the readback buffer" << std::endl;
            return false;
        }

        {
            CommandEncoderRef encoder(wgpuDeviceCreateCommandEncoder(state.device.Get(), nullptr));
            if (!encoder)
            {
                std::cerr << "WebGPU renderer: could not create a command encoder for the readback" << std::endl;
                return false;
            }
            WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            source.texture = texture;
            source.mipLevel = mipLevel;
            source.aspect = aspect;
            WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            destination.buffer = buffer.Get();
            destination.layout.offset = 0;
            destination.layout.bytesPerRow = paddedRowBytes;
            destination.layout.rowsPerImage = height;
            const WGPUExtent3D extent = {width, height, 1};
            wgpuCommandEncoderCopyTextureToBuffer(encoder.Get(), &source, &destination, &extent);
            CommandBufferRef commands(wgpuCommandEncoderFinish(encoder.Get(), nullptr));
            if (!commands)
            {
                std::cerr << "WebGPU renderer: could not finish the readback commands" << std::endl;
                return false;
            }
            const WGPUCommandBuffer handle = commands.Get();
            wgpuQueueSubmit(state.queue.Get(), 1, &handle);
        }

        // NATIVE ONLY: block until the buffer is mapped (in a browser this wait cannot happen: the map completes in a later
        // task, so a browser readback has to be asynchronous)
        auto result = std::make_shared<MapResult>();
        WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = OnMapped;
        callback.userdata1 = NewUserdata(result);
        const WGPUFuture future =
            wgpuBufferMapAsync(buffer.Get(), WGPUMapMode_Read, 0, static_cast<std::size_t>(size), callback);
        if (!WaitFor(state.instance.Get(), future, MapTimeoutNanoseconds) || !result->done)
        {
            std::cerr << "WebGPU renderer: mapping the readback buffer did not finish within 10 seconds" << std::endl;
            return false;
        }
        if (result->status != WGPUMapAsyncStatus_Success)
        {
            std::cerr << "WebGPU renderer: mapping the readback buffer failed: " << result->message << std::endl;
            return false;
        }
        const auto* mapped = static_cast<const std::uint8_t*>(
            wgpuBufferGetConstMappedRange(buffer.Get(), 0, static_cast<std::size_t>(size)));
        if (mapped == nullptr)
        {
            wgpuBufferUnmap(buffer.Get());
            std::cerr << "WebGPU renderer: the readback buffer has no mapped range" << std::endl;
            return false;
        }
        // Strip the row padding: tightly packed rows, row 0 the top of the image
        pixels.resize(static_cast<std::size_t>(rowBytes) * height);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                        mapped + static_cast<std::size_t>(y) * paddedRowBytes, rowBytes);
        }
        wgpuBufferUnmap(buffer.Get());
        return true;
    }

    bool WebGpuRenderer::CopyBufferToHostLocked(WGPUBuffer buffer, std::uint64_t size,
                                                std::vector<std::uint8_t>& bytes) const
    {
        State& state = *_state;
        if (size == 0 || size % 4 != 0 || size > state.maxBufferSize)
        {
            std::cerr << "WebGPU renderer: the buffer readback is not a non-zero multiple of 4 bytes within the device's "
                         "buffer limit"
                      << std::endl;
            return false;
        }
        WGPUBufferDescriptor bufferDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        bufferDescriptor.label = ToView("N2Engine buffer readback");
        bufferDescriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bufferDescriptor.size = size;
        BufferRef staging(wgpuDeviceCreateBuffer(state.device.Get(), &bufferDescriptor));
        if (!staging)
        {
            std::cerr << "WebGPU renderer: could not create the readback buffer" << std::endl;
            return false;
        }
        {
            CommandEncoderRef encoder(wgpuDeviceCreateCommandEncoder(state.device.Get(), nullptr));
            if (!encoder)
            {
                std::cerr << "WebGPU renderer: could not create a command encoder for the readback" << std::endl;
                return false;
            }
            wgpuCommandEncoderCopyBufferToBuffer(encoder.Get(), buffer, 0, staging.Get(), 0, size);
            CommandBufferRef commands(wgpuCommandEncoderFinish(encoder.Get(), nullptr));
            if (!commands)
            {
                std::cerr << "WebGPU renderer: could not finish the readback commands" << std::endl;
                return false;
            }
            const WGPUCommandBuffer handle = commands.Get();
            wgpuQueueSubmit(state.queue.Get(), 1, &handle);
        }

        // NATIVE ONLY: block until the buffer is mapped (see CopyTextureToHostLocked)
        auto result = std::make_shared<MapResult>();
        WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = OnMapped;
        callback.userdata1 = NewUserdata(result);
        const WGPUFuture future =
            wgpuBufferMapAsync(staging.Get(), WGPUMapMode_Read, 0, static_cast<std::size_t>(size), callback);
        if (!WaitFor(state.instance.Get(), future, MapTimeoutNanoseconds) || !result->done ||
            result->status != WGPUMapAsyncStatus_Success)
        {
            std::cerr << "WebGPU renderer: mapping the readback buffer did not succeed (" << result->message << ")"
                      << std::endl;
            return false;
        }
        const auto* mapped = static_cast<const std::uint8_t*>(
            wgpuBufferGetConstMappedRange(staging.Get(), 0, static_cast<std::size_t>(size)));
        if (mapped == nullptr)
        {
            wgpuBufferUnmap(staging.Get());
            std::cerr << "WebGPU renderer: the readback buffer has no mapped range" << std::endl;
            return false;
        }
        bytes.assign(mapped, mapped + size);
        wgpuBufferUnmap(staging.Get());
        return true;
    }

    void WebGpuRenderer::ReadFramebuffer(std::uint8_t* buffer, int width, int height) const
    {
        if (buffer == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.color)
        {
            return;
        }

        const std::uint32_t sourceWidth = _targetWidth;
        const std::uint32_t sourceHeight = _targetHeight;
        std::vector<std::uint8_t> pixels;
        if (!CopyTextureToHostLocked(state.color.Get(), WGPUTextureAspect_All, 4, sourceWidth, sourceHeight, pixels))
        {
            std::cerr << "WebGPU renderer: ReadFramebuffer could not copy the frame" << std::endl;
            return;
        }

        // The texture's row 0 is the top of the picture; the interface wants the bottom row first. A different size is
        // resampled (nearest, from pixel centres), as the software and Vulkan renderers do.
        const std::uint8_t* source = pixels.data();
        const std::size_t outputWidth = static_cast<std::size_t>(width);
        const std::size_t outputHeight = static_cast<std::size_t>(height);
        if (outputWidth == sourceWidth && outputHeight == sourceHeight)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(sourceWidth) * 4;
            for (std::size_t y = 0; y < outputHeight; ++y)
            {
                std::memcpy(buffer + y * rowBytes, source + (outputHeight - 1 - y) * rowBytes, rowBytes);
            }
            return;
        }
        for (std::size_t y = 0; y < outputHeight; ++y)
        {
            const std::size_t sourceYFromBottom = (2 * y + 1) * sourceHeight / (2 * outputHeight);
            const std::size_t sourceRow = sourceHeight - 1 - sourceYFromBottom;
            for (std::size_t x = 0; x < outputWidth; ++x)
            {
                const std::size_t sourceX = (2 * x + 1) * sourceWidth / (2 * outputWidth);
                std::memcpy(buffer + (y * outputWidth + x) * 4, source + (sourceRow * sourceWidth + sourceX) * 4, 4);
            }
        }
    }

    bool WebGpuRenderer::ReadDepthForTest(std::vector<float>& depth, std::uint32_t* width, std::uint32_t* height) const
    {
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.depth)
        {
            return false;
        }
        std::vector<std::uint8_t> bytes;
        if (!CopyTextureToHostLocked(state.depth.Get(), WGPUTextureAspect_DepthOnly, 4, _targetWidth, _targetHeight, bytes))
        {
            return false;
        }
        depth.resize(static_cast<std::size_t>(_targetWidth) * _targetHeight);
        std::memcpy(depth.data(), bytes.data(), bytes.size());
        if (width != nullptr)
        {
            *width = _targetWidth;
        }
        if (height != nullptr)
        {
            *height = _targetHeight;
        }
        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Sizes
    // -----------------------------------------------------------------------------------------------------------------

    void WebGpuRenderer::Resize(std::uint32_t width, std::uint32_t height)
    {
        std::lock_guard lock(_mutex);
        if (width == 0 || height == 0)
        {
            return; // a minimised window
        }
        if (!_initialized || _state->lost.load() || _targetFixed || (width == _targetWidth && height == _targetHeight))
        {
            return;
        }
        if (!RecreateTargets(width, height))
        {
            std::cerr << "WebGPU renderer: could not resize the render target to " << width << "x" << height
                      << "; frames are skipped until it can" << std::endl;
        }
    }

    void WebGpuRenderer::OnResize(int width, int height)
    {
        if (width > 0 && height > 0)
        {
            Resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        }
    }

    bool WebGpuRenderer::SetRenderTargetSize(std::uint32_t width, std::uint32_t height)
    {
        std::lock_guard lock(_mutex);
        if (width == 0 || height == 0 || !_initialized || _state->lost.load())
        {
            return false;
        }
        _targetFixed = true;
        if (_state->color && width == _targetWidth && height == _targetHeight)
        {
            return true; // the same size again keeps the target
        }
        if (!RecreateTargets(width, height))
        {
            std::cerr << "WebGPU renderer: could not make a render target of " << width << "x" << height << std::endl;
            return false;
        }
        return true;
    }
}
