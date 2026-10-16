#include "engine/scripting/LuaFieldDeclaration.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "engine/scripting/bindings/LuaBindings.hpp"

namespace N2Engine::Scripting
{
    bool IsLuaComponentType(const std::string &type)
    {
        // Any type scripts can add by name (Rigidbody, BoxCollider, ...), or a name ending in "Component".
        // Only the latter used to count, so `type = "Rigidbody"` reference fields were silently skipped.
        const auto names = Bindings::GetScriptableComponentNames();
        return std::ranges::find(names, type) != names.end() ||
               (type.size() > 9 && type.ends_with("Component"));
    }

    FieldInfo MakeLuaFieldInfo(const std::string &name, const std::string &declaredType,
                               const LuaDefaultKind defaultKind)
    {
        FieldInfo info;
        info.name = name;
        info.displayName = DefaultDisplayName(info.name);
        info.container = "scriptData";

        if (declaredType == "GameObject")
        {
            info.kind = FieldKind::GameObjectRef;
            info.typeName = "GameObject";
        }
        else if (!declaredType.empty() && IsLuaComponentType(declaredType))
        {
            info.kind = FieldKind::ComponentRef;
            info.typeName = declaredType;
        }
        else if (declaredType == "string")
        {
            info.kind = FieldKind::String;
            info.typeName = "string";
        }
        else if (declaredType == "int" || declaredType == "integer")
        {
            info.kind = FieldKind::Int;
            info.typeName = "int";
        }
        else if (declaredType == "number" || declaredType == "float")
        {
            info.kind = FieldKind::Float;
            info.typeName = "float";
        }
        else if (declaredType == "bool" || declaredType == "boolean")
        {
            info.kind = FieldKind::Bool;
            info.typeName = "bool";
        }
        else if (declaredType == "Vector3")
        {
            info.kind = FieldKind::Vector3;
            info.typeName = "Vector3";
        }
        else if (defaultKind == LuaDefaultKind::Integer || defaultKind == LuaDefaultKind::Float)
        {
            // As ExtractSerializableFields stores it: a Lua integer is an integer
            const bool isInteger = defaultKind == LuaDefaultKind::Integer;
            info.kind = isInteger ? FieldKind::Int : FieldKind::Float;
            info.typeName = isInteger ? "int" : "float";
        }
        else if (defaultKind == LuaDefaultKind::Bool)
        {
            info.kind = FieldKind::Bool;
            info.typeName = "bool";
        }
        else if (defaultKind == LuaDefaultKind::String)
        {
            info.kind = FieldKind::String;
            info.typeName = "string";
        }
        else if (defaultKind == LuaDefaultKind::Vector3)
        {
            info.kind = FieldKind::Vector3;
            info.typeName = "Vector3";
        }
        // else Json (the FieldInfo's own default): whatever the data holds
        if (info.kind == FieldKind::Int)
        {
            // Stored as a JSON integer and injected as a Lua integer
            info.limits = {static_cast<double>(std::numeric_limits<std::int64_t>::lowest()),
                           static_cast<double>(std::numeric_limits<std::int64_t>::max())};
        }
        return info;
    }
}
