#include "engine/Layers.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace N2Engine
{
    namespace
    {
        struct Registry
        {
            std::array<std::string, Layers::Count> names;
            std::array<uint32_t, Layers::Count> collidesWith{};
        };

        constexpr std::array<std::pair<int, const char *>, 5> BuiltinNames{{
            {Layers::Default, "Default"},
            {Layers::TransparentFX, "TransparentFX"},
            {Layers::IgnoreRaycast, "Ignore Raycast"},
            {Layers::Water, "Water"},
            {Layers::UI, "UI"},
        }};

        Registry MakeDefaults()
        {
            Registry registry;
            for (const auto &[layer, name] : BuiltinNames)
            {
                registry.names[layer] = name;
            }
            registry.collidesWith.fill(Layers::AllLayers);
            return registry;
        }

        Registry &Get()
        {
            static Registry registry = MakeDefaults();
            return registry;
        }
    }

    bool Layers::IsReserved(const int layer)
    {
        return std::ranges::any_of(BuiltinNames, [layer](const auto &builtin) { return builtin.first == layer; });
    }

    int Layers::Clamp(const int64_t layer, const std::string_view context)
    {
        if (layer >= 0 && layer < Count)
        {
            return static_cast<int>(layer);
        }
        const int clamped = layer < 0 ? 0 : Count - 1;
        Logger::Warn(std::format("Layer {} on '{}' is out of range (0-{}), using {}", layer, context, Count - 1, clamped));
        return clamped;
    }

    bool Layers::SetName(const int layer, const std::string &name)
    {
        if (!IsValid(layer))
        {
            Logger::Warn(std::format("Layers::SetName: layer {} is out of range (0-{})", layer, Count - 1));
            return false;
        }
        if (IsReserved(layer))
        {
            Logger::Warn(std::format("Layers::SetName: layer {} ('{}') is built in and can't be renamed",
                                     layer, Get().names[layer]));
            return false;
        }
        // NameToLayer must be unambiguous
        if (const int existing = NameToLayer(name); !name.empty() && existing != -1 && existing != layer)
        {
            Logger::Warn(std::format("Layers::SetName: layer {} is already called '{}'", existing, name));
            return false;
        }
        Get().names[layer] = name;
        return true;
    }

    std::string Layers::LayerToName(const int layer)
    {
        return IsValid(layer) ? Get().names[layer] : std::string{};
    }

    int Layers::NameToLayer(const std::string_view name)
    {
        if (name.empty())
        {
            return -1;
        }
        const auto &names = Get().names;
        for (int layer = 0; layer < Count; ++layer)
        {
            if (names[layer] == name)
            {
                return layer;
            }
        }
        return -1;
    }

    uint32_t Layers::GetMask(const std::vector<std::string> &names)
    {
        uint32_t mask = 0;
        for (const auto &name : names)
        {
            mask |= MaskOf(NameToLayer(name));
        }
        return mask;
    }

    void Layers::SetCollision(const int a, const int b, const bool collide)
    {
        if (!IsValid(a) || !IsValid(b))
        {
            Logger::Warn(std::format("Layers::SetCollision: layers ({}, {}) are out of range (0-{})", a, b, Count - 1));
            return;
        }
        auto &matrix = Get().collidesWith;
        const uint32_t before = matrix[a];
        if (collide)
        {
            matrix[a] |= 1u << b;
            matrix[b] |= 1u << a;
        }
        else
        {
            matrix[a] &= ~(1u << b);
            matrix[b] &= ~(1u << a);
        }
        if (matrix[a] != before)
        {
            NotifyMatrixChanged();
        }
    }

    bool Layers::GetCollision(const int a, const int b)
    {
        return IsValid(a) && IsValid(b) && (Get().collidesWith[a] & (1u << b)) != 0;
    }

    uint32_t Layers::CollisionMaskFor(const int layer)
    {
        return IsValid(layer) ? Get().collidesWith[layer] : 0u;
    }

    nlohmann::json Layers::Serialize()
    {
        const Registry &registry = Get();
        nlohmann::json names = nlohmann::json::array();
        nlohmann::json matrix = nlohmann::json::array();
        for (int layer = 0; layer < Count; ++layer)
        {
            names.push_back(registry.names[layer]);
            matrix.push_back(registry.collidesWith[layer]);
        }
        return {{"layers", names}, {"collisionMatrix", matrix}};
    }

    bool Layers::Deserialize(const nlohmann::json &j)
    {
        // Validated into a copy first, so a bad block leaves the current layers as they were
        const auto reject = [](const std::string &reason)
        {
            Logger::Warn("Layers::Deserialize: " + reason + "; the layer settings were not changed");
            return false;
        };

        if (!j.is_object())
        {
            return reject("expected an object");
        }

        Registry loaded = MakeDefaults();

        if (const auto layers = j.find("layers"); layers != j.end())
        {
            if (!layers->is_array() || layers->size() > Count)
            {
                return reject(std::format("\"layers\" must be an array of at most {} names", Count));
            }
            for (int layer = 0; layer < static_cast<int>(layers->size()); ++layer)
            {
                const auto &name = (*layers)[layer];
                if (!name.is_string())
                {
                    return reject(std::format("layer {}'s name isn't a string", layer));
                }
                if (IsReserved(layer))
                {
                    continue; // built-in names stay
                }
                const auto &text = name.get_ref<const std::string &>();
                if (!text.empty() && std::ranges::find(loaded.names, text) != loaded.names.end())
                {
                    return reject(std::format("the name '{}' is used twice", text));
                }
                loaded.names[layer] = text;
            }
        }

        if (const auto matrix = j.find("collisionMatrix"); matrix != j.end())
        {
            if (!matrix->is_array() || matrix->size() != Count)
            {
                return reject(std::format("\"collisionMatrix\" must be an array of {} masks", Count));
            }
            for (int layer = 0; layer < Count; ++layer)
            {
                const auto &row = (*matrix)[layer];
                // Parsed text gives non-negative integers as unsigned, but JSON built in C++ from an int is
                // a signed integer; accept either when it's in 0..0xFFFFFFFF
                const bool inRange = row.is_number_unsigned()
                                         ? row.get<uint64_t>() <= AllLayers
                                         : row.is_number_integer() && row.get<int64_t>() >= 0 &&
                                           row.get<int64_t>() <= static_cast<int64_t>(AllLayers);
                if (!inRange)
                {
                    return reject(std::format("collision row {} isn't a 32-bit mask", layer));
                }
                loaded.collidesWith[layer] = static_cast<uint32_t>(row.get<uint64_t>());
            }
            // Symmetric, as the filter shader treats it: a pair collides only if both rows allow it
            for (int a = 0; a < Count; ++a)
            {
                for (int b = a + 1; b < Count; ++b)
                {
                    const bool collide = (loaded.collidesWith[a] & (1u << b)) && (loaded.collidesWith[b] & (1u << a));
                    if (!collide && ((loaded.collidesWith[a] & (1u << b)) || (loaded.collidesWith[b] & (1u << a))))
                    {
                        Logger::Warn(std::format("Layers::Deserialize: layers {} and {} disagree about colliding; "
                                                 "they won't collide", a, b));
                    }
                    if (!collide)
                    {
                        loaded.collidesWith[a] &= ~(1u << b);
                        loaded.collidesWith[b] &= ~(1u << a);
                    }
                }
            }
        }

        const bool matrixChanged = loaded.collidesWith != Get().collidesWith;
        Get() = std::move(loaded);
        if (matrixChanged)
        {
            NotifyMatrixChanged();
        }
        return true;
    }

    void Layers::ResetToDefaults()
    {
        const bool matrixChanged = Get().collidesWith != MakeDefaults().collidesWith;
        Get() = MakeDefaults();
        if (matrixChanged)
        {
            NotifyMatrixChanged();
        }
    }

    void Layers::NotifyMatrixChanged()
    {
        if (auto *backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->RefreshCollisionMatrix();
        }
    }
}
