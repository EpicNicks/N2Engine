#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/NetworkId.hpp"

namespace N2Engine::Network
{
    /// A hash of the structure of one prefab (its Prefab JSON, or the bare JSON of its root): for every object in
    /// pre-order its name, its component type names in order and its number of children. Not of the UUIDs (a project
    /// that builds its prefabs in code gives them a fresh random one in every process, and two peers must still agree)
    /// and not of the component values (nothing replicates values yet; the value fields come with field replication).
    /// Bounded: a prefab of more than 100,000 objects hashes as that fact.
    [[nodiscard]] std::uint64_t PrefabShapeHash(const nlohmann::json &prefab);

    /**
     * The registered prefabs of one session, frozen: sorted by name, so a prefab is named on the wire by its index
     * (a u16) in a table both peers built from the same registrations. Capture() takes a snapshot of
     * PrefabManager's registry; a prefab registered later is not in a table that was captured before it, and the
     * network layer refuses to spawn it. The table has at most kMaxPrefabs entries (the first ones by name when more are
     * registered, with a warning). Hash() covers the names and shapes, and goes into the join fingerprint.
     */
    class PrefabTable
    {
    public:
        struct Entry
        {
            std::string name;
            std::uint64_t shapeHash = 0;
        };

        PrefabTable() = default;
        /// Sorts by name, keeps the first of any repeated name and at most kMaxPrefabs entries
        explicit PrefabTable(std::vector<Entry> entries);

        /// The prefabs registered with PrefabManager now
        [[nodiscard]] static PrefabTable Capture();

        [[nodiscard]] std::size_t Size() const noexcept { return _entries.size(); }
        /// nullptr for an index past the end
        [[nodiscard]] const Entry *At(std::size_t index) const noexcept;
        [[nodiscard]] std::optional<std::uint16_t> IndexOf(std::string_view name) const;
        [[nodiscard]] std::uint64_t Hash() const noexcept { return _hash; }
        [[nodiscard]] const std::vector<Entry> &Entries() const noexcept { return _entries; }

    private:
        std::vector<Entry> _entries;
        std::uint64_t _hash = 0;
    };
}
