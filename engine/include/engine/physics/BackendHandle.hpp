#pragma once

#include <atomic>
#include <cstdint>

namespace N2Engine::Physics
{
    /**
     * The generation-counted 64-bit handle every GPU-capable feature backend uses (particles, soft bodies): the
     * upper 32 bits are the GENERATION of the backend that issued the handle, the lower 32 are the object's index
     * in that backend. 0 is never a valid handle.
     *
     * Every backend object takes a new generation from a process-wide counter when it is constructed, so a backend
     * that was replaced or recreated, even at the same address as the old one, does not know the handles of its
     * predecessor: they fail its generation check and are ignored, instead of naming one of the new backend's
     * objects. Each feature has its own counter (NextBackendGeneration<Tag>), so adding a feature never changes
     * the generations another feature's backends get.
     */
    using BackendHandle = std::uint64_t;
    inline constexpr BackendHandle kInvalidBackendHandle = 0;

    [[nodiscard]] constexpr BackendHandle MakeBackendHandle(const std::uint32_t generation, const std::uint32_t index)
    {
        return (static_cast<BackendHandle>(generation) << 32) | index;
    }

    [[nodiscard]] constexpr std::uint32_t BackendHandleGeneration(const BackendHandle handle)
    {
        return static_cast<std::uint32_t>(handle >> 32);
    }

    [[nodiscard]] constexpr std::uint32_t BackendHandleIndex(const BackendHandle handle)
    {
        return static_cast<std::uint32_t>(handle & 0xFFFFFFFFu);
    }

    /// True when `handle` is not 0 and carries `generation`
    [[nodiscard]] constexpr bool BackendHandleOwnedBy(const std::uint32_t generation, const BackendHandle handle)
    {
        return handle != kInvalidBackendHandle && BackendHandleGeneration(handle) == generation;
    }

    /// The next backend generation of the feature `Tag`: 1, 2, 3, ... for the life of the process (0 is never
    /// issued, so a handle of generation 0 belongs to no backend)
    template <typename Tag>
    [[nodiscard]] std::uint32_t NextBackendGeneration()
    {
        static std::atomic<std::uint32_t> counter{0};
        return counter.fetch_add(1) + 1;
    }
}
