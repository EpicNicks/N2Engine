#pragma once

#include <string>

#include <math/UUID.hpp>

namespace N2Engine::Scripting
{
    class LuaRuntime;

    /**
     * A prefab asset as Lua sees it (the `Prefab` type): what a `type = "Prefab"` field of SerializableFields holds, and
     * what GameObject:GetPrefab returns. It holds only the asset's UUID: nothing is loaded until Instantiate, so a scene
     * full of spawners does not load every prefab, and a prefab that is missing fails when it is used. Two handles are
     * equal when they name the same UUID. Since asset UUIDs are stable (stored in the .meta), a moved or renamed prefab
     * is still the same handle.
     */
    class PrefabHandle
    {
    public:
        explicit PrefabHandle(const Math::UUID &uuid) : _uuid(uuid) {}

        [[nodiscard]] const Math::UUID &GetUUID() const { return _uuid; }
        /// The asset is indexed and is a prefab (resource type "Prefab")
        [[nodiscard]] bool IsValid() const;
        /// Its res:// path, or "" when no indexed asset has the UUID
        [[nodiscard]] std::string GetPath() const;
        /// The file name without ".prefab", or "" when no indexed asset has the UUID
        [[nodiscard]] std::string GetName() const;

        bool operator==(const PrefabHandle &other) const = default;

    private:
        Math::UUID _uuid;
    };

    namespace Bindings
    {
        /// The `Prefab` type and Prefab.Instantiate (editor protocol 1.25.0). Bound only into the engine's runtime state
        /// (LuaRuntime): editor extensions run in sandboxes of their own and do not have it.
        void BindPrefab(LuaRuntime &runtime);
    }
}
