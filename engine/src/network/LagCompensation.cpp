#include "engine/network/LagCompensation.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace N2Engine::Network
{
    namespace
    {
        using Math::Vector3;
        bool Finite(const Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
        float Dot(const Vector3 &a, const Vector3 &b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }
        float Length(const Vector3 &v)
        {
            return std::sqrt(Dot(v, v));
        }
        bool Valid(const HistoricalHitbox &b)
        {
            const auto &q = b.rotation;
            const float n = q.LengthSquared();
            return b.object.IsValid() && Finite(b.centre) && Finite(b.endpoint) && Finite(b.halfExtents) &&
                   std::isfinite(b.radius) && b.radius > 0 && b.radius <= 1e6f && b.halfExtents.x > 0 &&
                   b.halfExtents.y > 0 && b.halfExtents.z > 0 && std::isfinite(n) && std::abs(n - 1) < 0.001f &&
                   (b.shape == HistoricalHitbox::Shape::Sphere || b.shape == HistoricalHitbox::Shape::Capsule ||
                    b.shape == HistoricalHitbox::Shape::Box);
        }
        // Distance and outward normal at the query centre; used for inside tests and sweep contact points.
        float Distance(const HistoricalHitbox &b, const Vector3 &p, Vector3 &normal)
        {
            Vector3 delta;
            if (b.shape == HistoricalHitbox::Shape::Box)
            {
                const auto local = b.rotation.Conjugate() * (p - b.centre);
                const Vector3 closest{std::clamp(local.x, -b.halfExtents.x, b.halfExtents.x),
                                      std::clamp(local.y, -b.halfExtents.y, b.halfExtents.y),
                                      std::clamp(local.z, -b.halfExtents.z, b.halfExtents.z)};
                delta = b.rotation * (local - closest);
                const float len = Length(delta);
                if (len > 0)
                    normal = delta / len;
                else
                {
                    const float x = b.halfExtents.x - std::abs(local.x), y = b.halfExtents.y - std::abs(local.y),
                                z = b.halfExtents.z - std::abs(local.z);
                    const Vector3 face = x <= y && x <= z ? Vector3{local.x < 0 ? -1.0f : 1.0f, 0, 0}
                                         : y <= z         ? Vector3{0, local.y < 0 ? -1.0f : 1.0f, 0}
                                                          : Vector3{0, 0, local.z < 0 ? -1.0f : 1.0f};
                    normal = b.rotation * face;
                }
                return len;
            }
            auto closest = b.centre;
            if (b.shape == HistoricalHitbox::Shape::Capsule)
            {
                const auto segment = b.endpoint - b.centre;
                const float len2 = Dot(segment, segment);
                if (len2 > 0)
                    closest += segment * std::clamp(Dot(p - b.centre, segment) / len2, 0.0f, 1.0f);
            }
            delta = p - closest;
            const float len = Length(delta);
            normal = len > 0 ? delta / len : Vector3{};
            return len - b.radius;
        }

        double Dot64(const Vector3 &a, const Vector3 &b)
        {
            return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
        }

        double Sphere(const Vector3 &origin, const Vector3 &direction, const Vector3 &centre, double radius)
        {
            const auto delta = origin - centre;
            const double c = Dot64(delta, delta) - radius * radius;
            if (c <= 0)
                return 0;
            const double b = Dot64(delta, direction);
            const double discriminant = b * b - c;
            if (discriminant < 0 || b > 0)
                return std::numeric_limits<double>::infinity();
            // Stable quadratic root for a distant sphere (avoids subtracting two almost equal numbers).
            return c / (-b + std::sqrt(discriminant));
        }

        double Capsule(const Vector3 &origin, const Vector3 &direction, const Vector3 &a, const Vector3 &b,
                       double radius)
        {
            double result = std::min(Sphere(origin, direction, a, radius), Sphere(origin, direction, b, radius));
            const auto segment = b - a, offset = origin - a;
            const double length2 = Dot64(segment, segment);
            if (length2 < 1e-18)
                return result;
            const double alongRay = Dot64(segment, direction), alongOrigin = Dot64(segment, offset);
            const double qa = length2 - alongRay * alongRay;
            const double qb = length2 * Dot64(direction, offset) - alongOrigin * alongRay;
            const double qc = length2 * (Dot64(offset, offset) - radius * radius) - alongOrigin * alongOrigin;
            const double discriminant = qb * qb - qa * qc;
            if (qa > 1e-12 * length2 && discriminant >= 0)
            {
                const double root = std::sqrt(discriminant);
                for (double t : {(-qb - root) / qa, (-qb + root) / qa})
                {
                    const double y = alongOrigin + t * alongRay;
                    if (t >= 0 && y >= 0 && y <= length2)
                        result = std::min(result, t);
                }
            }
            return result;
        }

        double Intersect(const HistoricalHitbox &box, const Vector3 &origin, const Vector3 &direction, float radius)
        {
            Vector3 normal;
            if (Distance(box, origin, normal) <= radius)
                return 0;
            if (box.shape == HistoricalHitbox::Shape::Sphere)
                return Sphere(origin, direction, box.centre, double(box.radius) + radius);
            if (box.shape == HistoricalHitbox::Shape::Capsule)
                return Capsule(origin, direction, box.centre, box.endpoint, double(box.radius) + radius);
            const auto inverse = box.rotation.Conjugate();
            const auto local = inverse * (origin - box.centre), ray = inverse * direction;
            const float o[]{local.x, local.y, local.z}, d[]{ray.x, ray.y, ray.z};
            const float h[]{box.halfExtents.x, box.halfExtents.y, box.halfExtents.z};
            double result = std::numeric_limits<double>::infinity();
            // Six face patches of the rounded box. The twelve edge capsules include all eight corner spheres.
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(d[axis]) > 1e-12f)
                    for (int sign : {-1, 1})
                    {
                        const double t = (sign * (double(h[axis]) + radius) - o[axis]) / d[axis];
                        const int b = (axis + 1) % 3, c = (axis + 2) % 3;
                        if (t >= 0 && std::abs(o[b] + t * d[b]) <= h[b] + 1e-7 &&
                            std::abs(o[c] + t * d[c]) <= h[c] + 1e-7)
                            result = std::min(result, t);
                    }
                if (radius > 0)
                    for (int signB : {-1, 1})
                        for (int signC : {-1, 1})
                        {
                            const int b = (axis + 1) % 3, c = (axis + 2) % 3;
                            float a[3]{}, e[3]{};
                            a[axis] = -h[axis];
                            e[axis] = h[axis];
                            a[b] = e[b] = signB * h[b];
                            a[c] = e[c] = signC * h[c];
                            result = std::min(result, Capsule(local, ray, Vector3{a[0], a[1], a[2]},
                                                              Vector3{e[0], e[1], e[2]}, radius));
                        }
            }
            return result;
        }
    } // namespace
    LagCompensation::LagCompensation(std::uint64_t rewind, std::size_t frames)
        : _maxRewind(std::min<std::uint64_t>(rewind, 1'000'000)), _maxFrames(std::clamp<std::size_t>(frames, 2, 256))
    {
    }

    bool LagCompensation::IsValid(const HistoricalHitbox &hitbox)
    {
        return Valid(hitbox);
    }

    bool LagCompensation::Capture(std::uint64_t now, std::span<const HistoricalHitbox> boxes)
    {
        if (boxes.size() > kMaxNetworkObjects || (!_frames.empty() && now < _frames.back().time))
            return false;
        std::set<std::pair<std::uint32_t, std::uint16_t>> ids;
        for (const auto &b : boxes)
            if (!Valid(b) || !ids.emplace(b.object.value, b.index).second)
                return false;
        Frame frame{now, {boxes.begin(), boxes.end()}};
        std::sort(frame.boxes.begin(), frame.boxes.end(), [](const auto &a, const auto &b) {
            return std::pair{a.object.value, a.index} < std::pair{b.object.value, b.index};
        });
        if (!_frames.empty() && _frames.back().time == now)
            _frames.back() = std::move(frame);
        else
            _frames.push_back(std::move(frame));
        // Keep one sample before the window for interpolation, subject to the absolute frame bound.
        while (_frames.size() > _maxFrames || (_frames.size() > 2 && now - _frames[1].time > _maxRewind))
            _frames.pop_front();
        return true;
    }
    void LagCompensation::Forget(NetworkId id)
    {
        for (auto &f : _frames)
            std::erase_if(f.boxes, [id](const auto &b) { return b.object == id; });
    }
    std::optional<LagCompensatedHit> LagCompensation::Cast(std::uint64_t now, std::uint64_t requested,
                                                           std::uint64_t rtt, const Vector3 &origin,
                                                           const Vector3 &direction, float maxDistance, float radius,
                                                           std::uint32_t layers, NetworkId ignore) const
    {
        const float length = Length(direction);
        if (_frames.empty() || !Finite(origin) || !Finite(direction) || !std::isfinite(length) || length < 1e-6f ||
            !std::isfinite(maxDistance) || maxDistance <= 0 || maxDistance > 1e7f || !std::isfinite(radius) ||
            radius < 0 || radius > 1e6f)
            return {};
        const auto rewind = std::min(_maxRewind, rtt);
        const auto time = std::clamp(requested, now - std::min(now, rewind), now);
        if (time < _frames.front().time || (time > _frames.back().time && time - _frames.back().time > _maxRewind))
            return {};
        const auto sampleTime = std::min(time, _frames.back().time);
        const auto upper = std::lower_bound(_frames.begin(), _frames.end(), sampleTime,
                                            [](const Frame &f, std::uint64_t t) { return f.time < t; });
        const Frame &after = *upper;
        const Frame &before = upper == _frames.begin() || after.time == sampleTime ? after : *std::prev(upper);
        const float alpha = before.time == after.time ? 0
                                                      : static_cast<float>(sampleTime - before.time) /
                                                            static_cast<float>(after.time - before.time);
        const auto dir = direction / length;
        std::optional<LagCompensatedHit> hit;
        for (const auto &a : before.boxes)
        {
            if (a.object == ignore || !(a.layers & layers))
                continue;
            HistoricalHitbox box = a;
            if (alpha > 0)
            {
                const auto found = std::lower_bound(
                    after.boxes.begin(), after.boxes.end(), std::pair{a.object.value, a.index},
                    [](const auto &b, const auto &key) { return std::pair{b.object.value, b.index} < key; });
                if (found == after.boxes.end() || found->object != a.object || found->index != a.index)
                    continue;
                if (a.shape == found->shape && a.teleportSerial == found->teleportSerial)
                {
                    box.centre = a.centre + (found->centre - a.centre) * alpha;
                    box.endpoint = a.endpoint + (found->endpoint - a.endpoint) * alpha;
                    box.halfExtents = a.halfExtents + (found->halfExtents - a.halfExtents) * alpha;
                    box.radius = a.radius + (found->radius - a.radius) * alpha;
                    box.rotation = Math::Quaternion::Slerp(a.rotation, found->rotation, alpha);
                }
            }
            const double candidate = Intersect(box, origin, dir, radius);
            if (!std::isfinite(candidate) || candidate > maxDistance || (hit && candidate >= hit->distance))
                continue;
            const float distance = static_cast<float>(candidate);
            Vector3 normal;
            Distance(box, origin + dir * distance, normal);
            if (distance == 0 || Length(normal) < 1e-6f)
                normal = dir * -1.0f;
            hit =
                LagCompensatedHit{a.object, a.index, distance, origin + dir * distance - normal * radius, normal, time};
        }
        return hit;
    }
} // namespace N2Engine::Network
