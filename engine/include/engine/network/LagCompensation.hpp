#pragma once

#include "engine/network/NetworkId.hpp"
#include "math/Quaternion.hpp"
#include "math/Vector3.hpp"
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace N2Engine::Network
{
    // World-space shapes. Capsule endpoints are centres of its end spheres; boxes have half extents.
    struct HistoricalHitbox
    {
        enum class Shape
        {
            Sphere,
            Capsule,
            Box
        };
        NetworkId object;
        std::uint16_t index = 0;
        Shape shape = Shape::Sphere;
        Math::Vector3 centre;
        Math::Quaternion rotation;
        Math::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
        Math::Vector3 endpoint;
        float radius = 0.5f;
        std::uint32_t layers = ~0u;
        std::uint32_t teleportSerial = 0;
    };

    struct LagCompensatedHit
    {
        NetworkId object;
        std::uint16_t hitbox = 0;
        float distance = 0;
        Math::Vector3 point;
        Math::Vector3 normal;
        std::uint64_t timeMicros = 0;
    };

    // Server-owned history; never moves live physics actors. At most 256 frames, 4096 shapes/frame.
    // Capture is atomic: malformed/duplicate shapes or a backwards time leave the history unchanged.
    class LagCompensation
    {
      public:
        explicit LagCompensation(std::uint64_t maxRewindMicros = 200'000, std::size_t maxFrames = 64);
        bool Capture(std::uint64_t nowMicros, std::span<const HistoricalHitbox> hitboxes);
        [[nodiscard]] static bool IsValid(const HistoricalHitbox &hitbox);
        void Forget(NetworkId object);
        void Clear()
        {
            _frames.clear();
        }
        [[nodiscard]] std::size_t FrameCount() const
        {
            return _frames.size();
        }
        // The host supplies measured RTT, never a client claim. Rewind <= min(cap, measured RTT).
        // No history before the bounded query time means no hit. Between ticks the latest pose is held.
        [[nodiscard]] std::optional<LagCompensatedHit> Cast(std::uint64_t nowMicros, std::uint64_t requestedMicros,
                                                            std::uint64_t measuredRttMicros,
                                                            const Math::Vector3 &origin, const Math::Vector3 &direction,
                                                            float maxDistance, float castRadius = 0,
                                                            std::uint32_t layers = ~0u, NetworkId ignore = {}) const;

      private:
        struct Frame
        {
            std::uint64_t time;
            std::vector<HistoricalHitbox> boxes;
        };
        std::uint64_t _maxRewind;
        std::size_t _maxFrames;
        std::deque<Frame> _frames;
    };
} // namespace N2Engine::Network
