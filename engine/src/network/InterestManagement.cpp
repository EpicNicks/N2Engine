#include "engine/network/InterestManagement.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/NetworkWorld.hpp"
#include <algorithm>
#include <cmath>

namespace N2Engine::Network
{
    namespace
    {
        bool Finite(const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
    } // namespace
    InterestGrid::Cell InterestGrid::Key(const Math::Vector3 &p)
    {
        auto cell = [](float v) {
            return static_cast<int>(std::clamp(std::floor(static_cast<double>(v) / 64.0), -1e8, 1e8));
        };
        return {cell(p.x), cell(p.y), cell(p.z)};
    }
    void InterestGrid::Rebuild(const NetworkWorld &world)
    {
        _entries.clear();
        _cells.clear();
        _maxRadius = 0;
        if (!world.HasScene())
            return;
        for (const auto &[id, object] : world.Objects())
        {
            const auto *p = object->GetGameObject().GetPositionable();
            Entry entry{p ? p->GetPosition() : Math::Vector3{}, object->GetOwner(), object->GetInterestPolicy(),
                        p != nullptr};
            entry.positioned = entry.positioned && Finite(entry.position);
            _entries.emplace(id, entry);
            if (entry.policy.relevance == Relevance::Radius && entry.positioned)
            {
                _cells[Key(entry.position)].push_back(id);
                _maxRadius = std::max(_maxRadius, entry.policy.radius);
            }
        }
    }
    std::set<std::uint32_t> InterestGrid::Visible(ClientId client, NetworkId observer) const
    {
        std::set<std::uint32_t> visible;
        for (const auto &[id, e] : _entries)
            if (e.policy.relevance == Relevance::Always || e.owner == client)
                visible.insert(id);
        const auto origin = _entries.find(observer.value);
        if (origin == _entries.end() || origin->second.owner != client || !origin->second.positioned)
            return visible;
        const auto &p = origin->second.position;
        auto test = [&](std::uint32_t id) {
            const auto &e = _entries.at(id);
            if (e.policy.relevance != Relevance::Radius || !e.positioned)
                return;
            const double x = double(e.position.x) - p.x, y = double(e.position.y) - p.y, z = double(e.position.z) - p.z;
            if (x * x + y * y + z * z <= double(e.policy.radius) * e.policy.radius)
                visible.insert(id);
        };
        const auto [x, y, z] = Key(p);
        const int extent = static_cast<int>(std::ceil(_maxRadius / 64)) + 1;
        if (extent > 8)
            for (const auto &[id, e] : _entries)
                test(id);
        else
            for (int a = x - extent; a <= x + extent; ++a)
                for (int b = y - extent; b <= y + extent; ++b)
                    for (int c = z - extent; c <= z + extent; ++c)
                        if (const auto f = _cells.find({a, b, c}); f != _cells.end())
                            for (auto id : f->second)
                                test(id);
        return visible;
    }
    float InterestGrid::Priority(std::uint32_t id) const
    {
        const auto found = _entries.find(id);
        return found == _entries.end() ? 1 : found->second.policy.priority;
    }
} // namespace N2Engine::Network
