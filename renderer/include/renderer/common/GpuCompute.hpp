#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// General-purpose GPU compute on a renderer's device, for simulations that run beside the drawing (the WebGPU particle
// and soft-body backends of the engine). No graphics-API type appears here: a renderer that can run compute returns an
// object of this interface from IRenderer::GetCompute (today only the WebGPU renderer), and code outside the renderer
// sees nothing but these ids. Kernels are WGSL compute shaders.
namespace Renderer::Common
{
    /// A buffer made by IGpuCompute::CreateBuffer. 0 is never a valid id; ids are never reused, so an id that outlived
    /// its buffer (or the device) names nothing and every call with it fails or is ignored
    using GpuBufferId = std::uint64_t;
    /// A compute pipeline made by IGpuCompute::CreateKernel; never 0, never reused
    using GpuKernelId = std::uint64_t;
    /// An asynchronous read started by IGpuCompute::RequestRead; never 0, never reused
    using GpuReadTicket = std::uint64_t;

    /// What a buffer is bound as
    enum class GpuBufferUsage : std::uint8_t
    {
        /// var<storage, read> or var<storage, read_write>; may also be copied from and written to
        Storage,
        /// var<uniform>; may also be written to
        Uniform
    };

    /// The type of one binding of a kernel (group 0, binding i, in the order given to CreateKernel)
    enum class GpuBindingKind : std::uint8_t
    {
        Uniform,
        ReadOnlyStorage,
        Storage
    };

    enum class GpuReadStatus : std::uint8_t
    {
        /// The copy has not reached the CPU yet: ask again later (in a browser, after control returned to its event loop)
        Pending,
        /// The bytes are in `out`; the ticket is finished
        Done,
        /// The read failed or the ticket is unknown (finished, cancelled, or made before a shutdown); the ticket is finished
        Failed
    };

    /**
     * Compute on the renderer's device. Calls take effect in the order they are made: a Dispatch is recorded and runs when
     * Submit (or any call that has to submit first: WriteBuffer, RequestRead) sends it, and a WriteBuffer made after a
     * Dispatch is seen only by later dispatches. Storage written by one dispatch is visible to the next. Every method may be
     * called from any thread (the renderer's lock), and every method with an unknown id fails quietly (false, 0, Failed).
     * After the renderer shuts down or its device is lost, IsUsable is false and every id made before is unknown.
     *
     * Reads are asynchronous everywhere, because a browser can't wait for the GPU: RequestRead copies a range of a buffer
     * as it will be once everything submitted before it has run, and PollRead hands the bytes over once the copy has
     * reached the CPU (usually a frame or two later).
     */
    class IGpuCompute
    {
    public:
        virtual ~IGpuCompute() = default;

        /// True while the device runs: initialized, not shut down, not lost
        [[nodiscard]] virtual bool IsUsable() const = 0;
        /// "WebGPU": what health rows and logs call it
        [[nodiscard]] virtual std::string_view GetName() const = 0;
        /// The largest buffer CreateBuffer accepts (the device's storage-binding limit), 0 when not usable
        [[nodiscard]] virtual std::uint64_t GetMaxBufferBytes() const = 0;

        /// A buffer of `bytes` bytes (a positive multiple of 4), all zero. 0 when not usable, for a bad size or one over
        /// GetMaxBufferBytes, or when the device refuses
        [[nodiscard]] virtual GpuBufferId CreateBuffer(GpuBufferUsage usage, std::uint64_t bytes) = 0;
        /// Ignores 0 and unknown ids. Work already submitted that uses the buffer still completes
        virtual void DestroyBuffer(GpuBufferId buffer) = 0;
        /// Writes `bytes` bytes of `data` at `offset` (both multiples of 4, inside the buffer). Submits recorded dispatches
        /// first, so they see the buffer as it was. False when not usable, for an unknown id or a range outside the buffer
        virtual bool WriteBuffer(GpuBufferId buffer, std::uint64_t offset, const void *data, std::uint64_t bytes) = 0;

        /**
         * A compute pipeline of the entry point `entryPoint` of the WGSL module `wgsl`, with one bind group (group 0) whose
         * binding i has the kind bindings[i]. A module text and entry point already made into a kernel with the same
         * bindings gives the same id (the pipeline is cached). 0 when not usable or when the shader or pipeline is invalid
         * (the reason is logged once).
         */
        [[nodiscard]] virtual GpuKernelId CreateKernel(std::string_view label, std::string_view wgsl,
                                                       std::string_view entryPoint,
                                                       std::span<const GpuBindingKind> bindings) = 0;

        /// Records a dispatch of `kernel` with buffers[i] bound at binding i (as many buffers as the kernel has bindings)
        /// and groupsX x groupsY x groupsZ workgroups. A zero count records nothing and succeeds. False when not usable, for
        /// an unknown kernel or buffer, the wrong number of buffers, or a count over the device's limit
        virtual bool Dispatch(GpuKernelId kernel, std::span<const GpuBufferId> buffers, std::uint32_t groupsX,
                              std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1) = 0;
        /// Sends the recorded dispatches to the GPU. True when there was nothing to send
        virtual bool Submit() = 0;

        /// Starts copying `bytes` bytes at `offset` (multiples of 4, inside the buffer) to the CPU, after everything
        /// recorded or submitted before (it submits first). 0 when not usable or for a bad range
        [[nodiscard]] virtual GpuReadTicket RequestRead(GpuBufferId buffer, std::uint64_t offset, std::uint64_t bytes) = 0;
        /// Done: `out` holds the bytes (resized to the read's size). Pending: `out` is untouched. Failed: `out` is
        /// untouched. Done and Failed finish the ticket
        virtual GpuReadStatus PollRead(GpuReadTicket ticket, std::vector<std::uint8_t> &out) = 0;
        /// Forgets a read (its copy completes unseen); ignores unknown tickets
        virtual void CancelRead(GpuReadTicket ticket) = 0;

        /// NATIVE ONLY, for tests: blocks until the GPU has finished everything submitted and every map has completed (up to
        /// a few seconds). False in a browser, where nothing can wait, and when not usable
        virtual bool WaitIdleForTest() = 0;
    };
}
