#pragma once

#include <string>

#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::Scripting
{
    /// What the default of a script's SerializableFields entry is, as far as the editor's field kinds care (the
    /// shorthand `speed = 5` is its own default, `{ type = "int", default = 5 }` has one under `default`)
    enum class LuaDefaultKind
    {
        Other,
        Integer,
        Float,
        Bool,
        String,
        Vector3
    };

    /// True for a type name a script can declare for a component reference: a component type scripts can add by name
    /// (Rigidbody, BoxCollider, ...), or a name ending in "Component"
    [[nodiscard]] bool IsLuaComponentType(const std::string &type);

    /// The editor field for one entry of a script's SerializableFields: `declaredType` is the entry's `type` string
    /// (empty when it has none), `defaultKind` what its default is. The one mapping LuaComponent::DescribeFields (a
    /// running instance) and the DeclarationEvaluator (a throwaway evaluation) both use, so the two cannot disagree.
    [[nodiscard]] FieldInfo MakeLuaFieldInfo(const std::string &name, const std::string &declaredType,
                                             LuaDefaultKind defaultKind);
}
