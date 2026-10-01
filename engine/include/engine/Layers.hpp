#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace N2Engine
{
    /**
     * The project's 32 layers (Unity's model): each GameObject is on one layer, a layer mask has bit
     * (1 << layer) set for each layer it includes, and a symmetric collision matrix says which layers'
     * colliders interact. Indices and the built-in names match Unity's, so ported masks line up.
     * Engine-wide and main-thread only; the physics backend copies what it needs into each shape.
     */
    class Layers
    {
    public:
        static constexpr int Count = 32;

        // Built-in layers; their names can't be changed. 3 and 6..31 are free for the game.
        static constexpr int Default = 0;
        static constexpr int TransparentFX = 1;
        static constexpr int IgnoreRaycast = 2;
        static constexpr int Water = 4;
        static constexpr int UI = 5;

        static constexpr uint32_t AllLayers = 0xFFFFFFFFu;
        /// What queries use when no mask is given: every layer except Ignore Raycast
        static constexpr uint32_t DefaultRaycastMask = ~(1u << IgnoreRaycast);

        [[nodiscard]] static constexpr bool IsValid(const int layer) { return layer >= 0 && layer < Count; }
        [[nodiscard]] static bool IsReserved(int layer);
        /// The mask holding just this layer (0 for an invalid index)
        [[nodiscard]] static constexpr uint32_t MaskOf(const int layer) { return IsValid(layer) ? 1u << layer : 0u; }
        /// A layer index brought into 0..31, with a warning naming context if it wasn't
        [[nodiscard]] static int Clamp(int64_t layer, std::string_view context);

        /// Names a user layer (an empty name clears it). Refused, with a warning, for a built-in or
        /// out-of-range layer, or a name another layer already has.
        static bool SetName(int layer, const std::string &name);
        /// The layer's name, or "" for an unnamed or out-of-range layer
        [[nodiscard]] static std::string LayerToName(int layer);
        /// The layer with this name (exact match), or -1
        [[nodiscard]] static int NameToLayer(std::string_view name);

        /// The mask of the named layers; unknown names are skipped (as in Unity)
        template <typename... Names>
        [[nodiscard]] static uint32_t GetMask(const Names &... names)
            requires (std::convertible_to<const Names &, std::string_view> && ...)
        {
            uint32_t mask = 0;
            ((mask |= MaskOf(NameToLayer(std::string_view(names)))), ...);
            return mask;
        }
        [[nodiscard]] static uint32_t GetMask(const std::vector<std::string> &names);

        /// Whether colliders on layers a and b interact (contacts and triggers). Symmetric: setting (a, b)
        /// also sets (b, a). Changing it updates existing physics shapes; pairs it rules out end with an Exit.
        static void SetCollision(int a, int b, bool collide);
        [[nodiscard]] static bool GetCollision(int a, int b);
        /// Every layer that collides with this one, as a mask (0 for an invalid index)
        [[nodiscard]] static uint32_t CollisionMaskFor(int layer);

        /// {"layers": [32 names], "collisionMatrix": [32 row masks]}
        [[nodiscard]] static nlohmann::json Serialize();
        /// Loads a block written by Serialize. Malformed input changes nothing: it's logged and false is
        /// returned (never throws). Built-in names are kept whatever the block says, and an asymmetric
        /// matrix is made symmetric (a pair collides only if both rows say so).
        static bool Deserialize(const nlohmann::json &j);

        /// Built-in names only and everything colliding (also for tests, which share the registry)
        static void ResetToDefaults();

    private:
        /// Lets the physics backend re-filter existing shapes after the matrix changed
        static void NotifyMatrixChanged();
    };
}
