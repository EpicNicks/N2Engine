#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <renderer/common/GpuCompute.hpp>

namespace N2Engine::TestSupport
{
    /// A Renderer::Common::IGpuCompute that runs nothing: what the backend selection tests hand SelectParticleBackend and
    /// SelectSoftBodyBackend in place of the WebGPU renderer's compute. `usable` is what IsUsable answers; every other call
    /// fails (0, false, Failed), so a WebGPU backend made on it is not healthy.
    class FakeGpuCompute final : public Renderer::Common::IGpuCompute
    {
    public:
        explicit FakeGpuCompute(const bool isUsable = true) : usable(isUsable) {}

        [[nodiscard]] bool IsUsable() const override { return usable; }
        [[nodiscard]] std::string_view GetName() const override { return "WebGPU"; }
        [[nodiscard]] std::uint64_t GetMaxBufferBytes() const override { return 0; }
        [[nodiscard]] Renderer::Common::GpuBufferId CreateBuffer(Renderer::Common::GpuBufferUsage, std::uint64_t) override
        {
            return 0;
        }
        void DestroyBuffer(Renderer::Common::GpuBufferId) override {}
        bool WriteBuffer(Renderer::Common::GpuBufferId, std::uint64_t, const void *, std::uint64_t) override
        {
            return false;
        }
        [[nodiscard]] Renderer::Common::GpuKernelId CreateKernel(std::string_view, std::string_view, std::string_view,
                                                                 std::span<const Renderer::Common::GpuBindingKind>) override
        {
            return 0;
        }
        bool Dispatch(Renderer::Common::GpuKernelId, std::span<const Renderer::Common::GpuBufferId>, std::uint32_t,
                      std::uint32_t, std::uint32_t) override
        {
            return false;
        }
        bool Submit() override { return false; }
        [[nodiscard]] Renderer::Common::GpuReadTicket RequestRead(Renderer::Common::GpuBufferId, std::uint64_t,
                                                                  std::uint64_t) override
        {
            return 0;
        }
        Renderer::Common::GpuReadStatus PollRead(Renderer::Common::GpuReadTicket, std::vector<std::uint8_t> &) override
        {
            return Renderer::Common::GpuReadStatus::Failed;
        }
        void CancelRead(Renderer::Common::GpuReadTicket) override {}
        bool WaitIdleForTest() override { return false; }

        bool usable = true;
    };
}
