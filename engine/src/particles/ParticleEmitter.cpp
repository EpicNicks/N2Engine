#include "engine/particles/ParticleEmitter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace N2Engine::Particles
{
    namespace
    {
        constexpr std::uint32_t kMaxPerStep = 4096;
        constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
    }

    void ParticleEmitter::Reset(const std::uint32_t seed)
    {
        _random.Seed(seed);
        _fraction = 0.0f;
    }

    std::uint32_t ParticleEmitter::Advance(const float rate, const float dt)
    {
        if (!(rate > 0.0f) || !(dt > 0.0f) || !std::isfinite(rate) || !std::isfinite(dt))
        {
            return 0;
        }
        _fraction += rate * dt;
        if (!std::isfinite(_fraction))
        {
            _fraction = 0.0f;
            return 0;
        }
        const float whole = std::floor(_fraction);
        _fraction -= whole;
        return static_cast<std::uint32_t>(std::min(whole, static_cast<float>(kMaxPerStep)));
    }

    void ParticleEmitter::Generate(const EmitterShapeSettings &settings, const Math::Vector3 &origin,
                                   const Math::Quaternion &rotation, const std::uint32_t count,
                                   std::vector<Math::Vector3> &positions, std::vector<Math::Vector3> &velocities)
    {
        positions.clear();
        velocities.clear();
        const float radius = std::max(0.0f, std::isfinite(settings.shapeRadius) ? settings.shapeRadius : 0.0f);
        const float coneAngle = std::clamp(std::isfinite(settings.coneAngle) ? settings.coneAngle : 0.0f, 0.0f, 180.0f);
        const float cosCone = std::cos(coneAngle * kDegToRad);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            // The same number of random draws for every shape, so changing the shape doesn't shift the speeds
            const float u0 = _random.NextFloat();
            const float u1 = _random.NextFloat();
            const float u2 = _random.NextFloat();
            const float u3 = _random.NextFloat();
            const float speed = _random.Range(settings.speedMin, settings.speedMax);

            Math::Vector3 localPosition;
            Math::Vector3 localDirection;
            if (settings.shape == EmitterShape::Sphere)
            {
                // A uniform direction (z uniform in [-1, 1], angle uniform), a radius with a uniform volume density
                const float z = 2.0f * u0 - 1.0f;
                const float ring = std::sqrt(std::max(0.0f, 1.0f - z * z));
                const float angle = kTwoPi * u1;
                localDirection = Math::Vector3(ring * std::cos(angle), z, ring * std::sin(angle));
                const float r = radius * std::cbrt(u2);
                localPosition = Math::Vector3(localDirection.x * r, localDirection.y * r, localDirection.z * r);
            }
            else
            {
                // A point on the disc (uniform area), a direction uniform over the cap the cone cuts from the sphere
                const float discR = radius * std::sqrt(u0);
                const float discAngle = kTwoPi * u1;
                localPosition = Math::Vector3(discR * std::cos(discAngle), 0.0f, discR * std::sin(discAngle));
                const float cosPolar = 1.0f - u2 * (1.0f - cosCone);
                const float sinPolar = std::sqrt(std::max(0.0f, 1.0f - cosPolar * cosPolar));
                const float azimuth = kTwoPi * u3;
                localDirection = Math::Vector3(sinPolar * std::cos(azimuth), cosPolar, sinPolar * std::sin(azimuth));
            }

            const Math::Vector3 worldOffset = rotation * localPosition;
            const Math::Vector3 worldDirection = rotation * localDirection;
            positions.emplace_back(origin.x + worldOffset.x, origin.y + worldOffset.y, origin.z + worldOffset.z);
            velocities.emplace_back(worldDirection.x * speed, worldDirection.y * speed, worldDirection.z * speed);
        }
    }
}
