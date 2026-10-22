#include "engine/network/PrefabTable.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "engine/Logger.hpp"
#include "engine/network/NetworkHash.hpp"
#include "engine/prefabs/PrefabManager.hpp"

namespace N2Engine::Network
{
    std::uint64_t PrefabShapeHash(const nlohmann::json &prefab)
    {
        constexpr std::size_t kMaxObjects = 100000;

        const nlohmann::json *root = &prefab;
        if (prefab.is_object())
        {
            if (const auto wrapped = prefab.find("rootObject"); wrapped != prefab.end())
            {
                root = &*wrapped;
            }
        }

        Fnv1a64 hash;
        // A stack on the heap, not recursion: prefab data can nest as deep as JsonLimits allows
        std::vector<const nlohmann::json *> pending{root};
        std::size_t visited = 0;
        while (!pending.empty())
        {
            const nlohmann::json &node = *pending.back();
            pending.pop_back();
            if (++visited > kMaxObjects)
            {
                hash.String("more than 100000 objects");
                break;
            }
            if (!node.is_object())
            {
                hash.U8(0);
                continue;
            }
            hash.U8(1);

            const auto name = node.find("name");
            hash.String(name != node.end() && name->is_string() ? std::string_view(name->get_ref<const std::string &>())
                                                                 : std::string_view());

            const auto components = node.find("components");
            if (components != node.end() && components->is_array())
            {
                hash.U32(static_cast<std::uint32_t>(components->size()));
                for (const nlohmann::json &component : *components)
                {
                    const auto type = component.is_object() ? component.find("type") : component.end();
                    hash.String(component.is_object() && type != component.end() && type->is_string()
                                    ? std::string_view(type->get_ref<const std::string &>())
                                    : std::string_view());
                }
            }
            else
            {
                hash.U32(0);
            }

            const auto children = node.find("children");
            if (children != node.end() && children->is_array())
            {
                hash.U32(static_cast<std::uint32_t>(children->size()));
                // Reversed, so the first child is the next one popped
                for (auto child = children->rbegin(); child != children->rend(); ++child)
                {
                    pending.push_back(&*child);
                }
            }
            else
            {
                hash.U32(0);
            }
        }
        return hash.Value();
    }

    PrefabTable::PrefabTable(std::vector<Entry> entries) : _entries(std::move(entries))
    {
        std::stable_sort(_entries.begin(), _entries.end(),
                         [](const Entry &a, const Entry &b) { return a.name < b.name; });
        _entries.erase(std::unique(_entries.begin(), _entries.end(),
                                   [](const Entry &a, const Entry &b) { return a.name == b.name; }),
                       _entries.end());
        if (_entries.size() > kMaxPrefabs)
        {
            _entries.resize(kMaxPrefabs);
        }

        Fnv1a64 hash;
        hash.U32(static_cast<std::uint32_t>(_entries.size()));
        for (const Entry &entry : _entries)
        {
            hash.String(entry.name);
            hash.U64(entry.shapeHash);
        }
        _hash = hash.Value();
    }

    PrefabTable PrefabTable::Capture()
    {
        std::vector<Entry> entries;
        const std::vector<std::string> names = PrefabManager::GetRegisteredPrefabNames();
        for (const std::string &name : names)
        {
            if (entries.size() >= kMaxPrefabs)
            {
                Logger::Warn(std::format("PrefabTable: {} prefabs are registered; only the first {} by name are in the "
                                         "network prefab table",
                                         names.size(), kMaxPrefabs));
                break;
            }
            const std::optional<nlohmann::json> data = PrefabManager::GetPrefabData(name);
            if (!data.has_value())
            {
                continue;
            }
            entries.push_back(Entry{name, PrefabShapeHash(*data)});
        }
        return PrefabTable(std::move(entries));
    }

    const PrefabTable::Entry *PrefabTable::At(const std::size_t index) const noexcept
    {
        return index < _entries.size() ? &_entries[index] : nullptr;
    }

    std::optional<std::uint16_t> PrefabTable::IndexOf(const std::string_view name) const
    {
        const auto found = std::lower_bound(_entries.begin(), _entries.end(), name,
                                            [](const Entry &entry, const std::string_view key)
                                            { return std::string_view(entry.name) < key; });
        if (found == _entries.end() || std::string_view(found->name) != name)
        {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(found - _entries.begin());
    }
}
