#pragma once
#include "engine/network/NetworkId.hpp"
#include "math/Vector3.hpp"
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace N2Engine::Network
{
    enum class Relevance
    {
        Always,
        OwnerOnly,
        Radius
    };
    struct InterestPolicy
    {
        Relevance relevance = Relevance::Always;
        float radius = 100;
        float priority = 1;
    };
    class NetworkWorld;
    // Uniform grid rebuilt once per send tick. Large volumes fall back to the bounded object list.
    class InterestGrid
    {
      public:
        void Rebuild(const NetworkWorld &world);
        [[nodiscard]] std::set<std::uint32_t> Visible(ClientId client, NetworkId observer) const;
        [[nodiscard]] float Priority(std::uint32_t id) const;

      private:
        using Cell = std::tuple<int, int, int>;
        struct Entry
        {
            Math::Vector3 position;
            ClientId owner;
            InterestPolicy policy;
            bool positioned;
        };
        static Cell Key(const Math::Vector3 &p);
        std::map<std::uint32_t, Entry> _entries;
        std::map<Cell, std::vector<std::uint32_t>> _cells;
        float _maxRadius = 0;
    };
} // namespace N2Engine::Network
