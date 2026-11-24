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
        _spinRandom.Seed(SpinSeed(seed));
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
                                   std::vector<Math::Vector3> &positions, std::vector<Math::Vector3> &velocities,
                                   std::vector<Math::Vector2> *spins)
    {
        positions.clear();
        velocities.clear();
        if (spins)
        {
            spins->clear();
        }
        const float radius = std::max(0.0f, std::isfinite(settings.shapeRadius) ? settings.shapeRadius : 0.0f);
        const float coneAngle = std::clamp(std::isfinite(settings.coneAngle) ? settings.coneAngle : 0.0f, 0.0f, 180.0f);
        const float cosCone = std::cos(coneAngle * kDegToRad);
        const bool edge = settings.emitFromEdge;
        const auto half = [](const float size) { return std::isfinite(size) ? 0.5f * std::abs(size) : 0.0f; };
        const float hx = half(settings.boxSize.x);
        const float hy = half(settings.boxSize.y);
        const float hz = half(settings.boxSize.z);
        const auto finite = [](const float value) { return std::isfinite(value) ? value : 0.0f; };
        const float rotationMin = finite(settings.startRotationMin) * kDegToRad;
        const float rotationMax = finite(settings.startRotationMax) * kDegToRad;
        const float spinMin = finite(settings.angularVelocityMin) * kDegToRad;
        const float spinMax = finite(settings.angularVelocityMax) * kDegToRad;

        for (std::uint32_t i = 0; i < count; ++i)
        {
            // The same number of random draws for every shape, so changing the shape doesn't shift the speeds
            const float u0 = _random.NextFloat();
            const float u1 = _random.NextFloat();
            const float u2 = _random.NextFloat();
            const float u3 = _random.NextFloat();
            const float speed = _random.Range(settings.speedMin, settings.speedMax);

            // A direction uniform over the cap the cone cuts from the sphere around local +Y (Cone and Box)
            const auto coneDirection = [&]
            {
                const float cosPolar = 1.0f - u2 * (1.0f - cosCone);
                const float sinPolar = std::sqrt(std::max(0.0f, 1.0f - cosPolar * cosPolar));
                const float azimuth = kTwoPi * u3;
                return Math::Vector3(sinPolar * std::cos(azimuth), cosPolar, sinPolar * std::sin(azimuth));
            };

            Math::Vector3 localPosition;
            Math::Vector3 localDirection;
            switch (settings.shape)
            {
            case EmitterShape::Sphere:
            {
                // A uniform direction (z uniform in [-1, 1], angle uniform), a radius with a uniform volume density (or
                // the radius itself on the surface)
                const float z = 2.0f * u0 - 1.0f;
                const float ring = std::sqrt(std::max(0.0f, 1.0f - z * z));
                const float angle = kTwoPi * u1;
                localDirection = Math::Vector3(ring * std::cos(angle), z, ring * std::sin(angle));
                const float r = edge ? radius : radius * std::cbrt(u2);
                localPosition = Math::Vector3(localDirection.x * r, localDirection.y * r, localDirection.z * r);
                break;
            }
            case EmitterShape::Circle:
            {
                // A point on the disc in the XY plane (uniform area) or on its rim, moving away from the centre in the plane
                const float angle = kTwoPi * u1;
                const float r = edge ? radius : radius * std::sqrt(u0);
                localDirection = Math::Vector3(std::cos(angle), std::sin(angle), 0.0f);
                localPosition = Math::Vector3(localDirection.x * r, localDirection.y * r, 0.0f);
                break;
            }
            case EmitterShape::Edge:
            {
                // A point on the line, a direction within coneAngle of +Y in the XY plane
                const float angle = (2.0f * u2 - 1.0f) * coneAngle * kDegToRad;
                localPosition = Math::Vector3((2.0f * u0 - 1.0f) * radius, 0.0f, 0.0f);
                localDirection = Math::Vector3(std::sin(angle), std::cos(angle), 0.0f);
                break;
            }
            case EmitterShape::Box:
            {
                // One more draw than the other shapes (the third coordinate, or which face): only Box takes it
                const float u4 = _random.NextFloat();
                localDirection = coneDirection();
                const float areaX = hy * hz;
                const float areaY = hx * hz;
                const float areaZ = hx * hy;
                const float total = areaX + areaY + areaZ;
                if (!edge || !(total > 0.0f))
                {
                    localPosition = Math::Vector3((2.0f * u0 - 1.0f) * hx, (2.0f * u1 - 1.0f) * hy, (2.0f * u4 - 1.0f) * hz);
                    break;
                }
                // A face, chosen by its area: u4 picks the axis, and the first or second half of that axis's share the side
                const float a = (2.0f * u0 - 1.0f);
                const float b = (2.0f * u1 - 1.0f);
                float pick = u4 * total;
                if (pick < areaX)
                {
                    localPosition = Math::Vector3(pick < 0.5f * areaX ? -hx : hx, a * hy, b * hz);
                }
                else if ((pick -= areaX) < areaY)
                {
                    localPosition = Math::Vector3(a * hx, pick < 0.5f * areaY ? -hy : hy, b * hz);
                }
                else
                {
                    pick -= areaY;
                    localPosition = Math::Vector3(a * hx, b * hy, pick < 0.5f * areaZ ? -hz : hz);
                }
                break;
            }
            case EmitterShape::Cone:
            default:
            {
                // A point on the disc (uniform area) or its rim, a direction uniform over the cap the cone cuts from the
                // sphere
                const float discR = edge ? radius : radius * std::sqrt(u0);
                const float discAngle = kTwoPi * u1;
                localPosition = Math::Vector3(discR * std::cos(discAngle), 0.0f, discR * std::sin(discAngle));
                localDirection = coneDirection();
                break;
            }
            }

            const Math::Vector3 worldOffset = rotation * localPosition;
            const Math::Vector3 worldDirection = rotation * localDirection;
            positions.emplace_back(origin.x + worldOffset.x, origin.y + worldOffset.y, origin.z + worldOffset.z);
            velocities.emplace_back(worldDirection.x * speed, worldDirection.y * speed, worldDirection.z * speed);
            if (spins)
            {
                const float start = _spinRandom.Range(std::min(rotationMin, rotationMax), std::max(rotationMin, rotationMax));
                const float spin = _spinRandom.Range(std::min(spinMin, spinMax), std::max(spinMin, spinMax));
                spins->emplace_back(start, spin);
            }
        }
    }
}
