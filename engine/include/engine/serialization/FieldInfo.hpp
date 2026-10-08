#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Quaternion.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "engine/common/Color.hpp"

namespace N2Engine
{
    /**
     * What an editor shows a serialized member as. The kind decides the editor (a checkbox, a drag field, a colour
     * picker, an asset slot); the value always travels as the JSON Serialize() writes for the member.
     */
    enum class FieldKind
    {
        Bool,
        Int,
        Float,
        String,
        Vector2,
        Vector3,
        Vector4,
        Quaternion,
        /// A colour: Common::Color ({r,g,b,a}), or a Vector3/Vector4 marked with FieldBuilder::AsColor (typeName says
        /// which JSON shape it is)
        Color,
        /// An enum registered with N2_SERIALIZE_ENUM: a string, one of enumOptions
        Enum,
        /// A UUID string (or null) naming an asset of assetType
        AssetRef,
        /// An array of those
        AssetRefList,
        /// A UUID string (or null) naming a GameObject of the scene
        GameObjectRef,
        /// An array of those
        GameObjectRefList,
        /// A UUID string (or null) naming a component of the scene
        ComponentRef,
        /// An array of those
        ComponentRefList,
        /// Anything else the member's own to_json writes: shown as a JSON editor, validated by its from_json
        Json
    };

    /// The kind's name as it is on the wire ("Bool", "Vector3", "AssetRefList", ...)
    [[nodiscard]] std::string_view FieldKindName(FieldKind kind);

    /**
     * Everything an editor needs to show and edit one serialized member. Built at the RegisterMember call from the
     * member's C++ type (FieldTraits), then refined by the opt-in FieldBuilder calls.
     */
    struct FieldInfo
    {
        /// The JSON key (what Serialize writes), inside `container` when that isn't empty
        std::string name;
        /// "_playOnAwake" is "Play On Awake" unless FieldBuilder::DisplayName says otherwise
        std::string displayName;
        FieldKind kind = FieldKind::Json;
        /// The C++ side's name for the type: "float", "Vector3", "LightType", "Font", "Component", "json"...
        std::string typeName = "json";
        /// Enum only: the names a value can take, in declaration order
        std::vector<std::string> enumOptions;
        /// AssetRef and AssetRefList: the resource type the asset must have ("Font", "Texture", ...); empty when the
        /// asset type doesn't say
        std::string assetType;
        /// Int and Float: a value set through the editor is clamped to it
        std::optional<std::pair<double, double>> range;
        std::string tooltip;
        /// Not empty: the field lives in the object under this key of the component's JSON, not at its top level
        /// (a LuaComponent's script fields are under "scriptData"). A reference there is written {"$ref": uuid}.
        std::string container;
        bool hidden = false;
        bool readOnly = false;

        /// The FieldSchema of the editor protocol (protocol.json): optional keys are left out when empty
        [[nodiscard]] nlohmann::json ToJson() const;
    };

    /// "_playOnAwake" -> "Play On Awake", "sortOrder" -> "Sort Order", "UIText" stays "UI Text"
    [[nodiscard]] std::string DefaultDisplayName(std::string_view name);

    /**
     * Whether `value` is a well-formed value for the field, as a message naming the field when it isn't. Checks the
     * JSON's shape against the kind (a number for Float, {x,y,z} for Vector3, a name from enumOptions for Enum, a
     * UUID string or null for a reference); a value the member's own from_json judges (Json) always passes here.
     */
    [[nodiscard]] std::optional<std::string> ValidateFieldValue(const FieldInfo &field, const nlohmann::json &value);

    /// A number clamped to the field's range, rounded for Int; any other value, and a field without a range,
    /// unchanged
    [[nodiscard]] nlohmann::json ClampFieldValue(const FieldInfo &field, const nlohmann::json &value);

    /**
     * Checks a (partial) object of values against a component's fields, with ValidateFieldValue: every key has to be
     * a field (or a container's field, or "isActive"), a field mustn't be read-only, and nothing is applied by this.
     * Returns the first problem, or nullopt.
     */
    [[nodiscard]] std::optional<std::string> ValidateFieldValues(const std::vector<FieldInfo> &fields,
                                                                 const nlohmann::json &values);

    // ==================== FieldTraits ====================

    /// A member's FieldKind and type name from its C++ type. Anything this doesn't list is Json.
    template <typename T>
    struct FieldTraits
    {
        static constexpr FieldKind kind = FieldKind::Json;
        static std::string TypeName() { return "json"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<bool>
    {
        static constexpr FieldKind kind = FieldKind::Bool;
        static std::string TypeName() { return "bool"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <std::integral T>
    struct FieldTraits<T>
    {
        static constexpr FieldKind kind = FieldKind::Int;
        static std::string TypeName() { return sizeof(T) > 4 ? "int64" : "int"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <std::floating_point T>
    struct FieldTraits<T>
    {
        static constexpr FieldKind kind = FieldKind::Float;
        static std::string TypeName() { return std::is_same_v<T, float> ? "float" : "double"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<std::string>
    {
        static constexpr FieldKind kind = FieldKind::String;
        static std::string TypeName() { return "string"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<Math::Vector2>
    {
        static constexpr FieldKind kind = FieldKind::Vector2;
        static std::string TypeName() { return "Vector2"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<Math::Vector3>
    {
        static constexpr FieldKind kind = FieldKind::Vector3;
        static std::string TypeName() { return "Vector3"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<Math::Vector4>
    {
        static constexpr FieldKind kind = FieldKind::Vector4;
        static std::string TypeName() { return "Vector4"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<Math::Quaternion>
    {
        static constexpr FieldKind kind = FieldKind::Quaternion;
        static std::string TypeName() { return "Quaternion"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    template <>
    struct FieldTraits<Common::Color>
    {
        static constexpr FieldKind kind = FieldKind::Color;
        static std::string TypeName() { return "Color"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };

    /// An enum registered with N2_SERIALIZE_ENUM: found by argument-dependent lookup of N2EnumOptions and
    /// N2EnumTypeName in the enum's own namespace. An enum registered only with NLOHMANN_JSON_SERIALIZE_ENUM is Json.
    template <typename T>
        requires(std::is_enum_v<T> && requires { N2EnumOptions(static_cast<const T *>(nullptr)); })
    struct FieldTraits<T>
    {
        static constexpr FieldKind kind = FieldKind::Enum;
        static std::string TypeName() { return N2EnumTypeName(static_cast<const T *>(nullptr)); }
        static std::vector<std::string> EnumOptions() { return N2EnumOptions(static_cast<const T *>(nullptr)); }
    };

    /// The FieldInfo a member of type T starts with (RegisterMember)
    template <typename T>
    [[nodiscard]] FieldInfo MakeFieldInfo(const std::string &name)
    {
        FieldInfo info;
        info.name = name;
        info.displayName = DefaultDisplayName(name);
        info.kind = FieldTraits<T>::kind;
        info.typeName = FieldTraits<T>::TypeName();
        info.enumOptions = FieldTraits<T>::EnumOptions();
        return info;
    }

    /// The resource type of an asset class: its static ResourceTypeName, or empty for a class without one
    template <typename T>
    [[nodiscard]] std::string AssetTypeNameOf()
    {
        if constexpr (requires { std::string(T::ResourceTypeName); })
        {
            return std::string(T::ResourceTypeName);
        }
        else
        {
            return {};
        }
    }
}

/**
 * NLOHMANN_JSON_SERIALIZE_ENUM, and the names of its values for an editor's drop-down. Use it in the enum's namespace
 * (it defines two functions there, found through ADL), with the same list:
 *
 *     N2_SERIALIZE_ENUM(LightType, {
 *         { LightType::Directional, "Directional" },
 *         { LightType::Point, "Point" }
 *     })
 *
 * The options are the names in list order. As with NLOHMANN_JSON_SERIALIZE_ENUM, the first entry is what an unknown
 * name loads as; the editor protocol rejects a name that isn't an option before it gets that far.
 */
#define N2_SERIALIZE_ENUM(ENUM_TYPE, ...)                                                                          \
    NLOHMANN_JSON_SERIALIZE_ENUM(ENUM_TYPE, __VA_ARGS__)                                                           \
    [[maybe_unused]] inline std::string N2EnumTypeName(const ENUM_TYPE *)                                          \
    {                                                                                                              \
        return #ENUM_TYPE;                                                                                         \
    }                                                                                                              \
    [[maybe_unused]] inline std::vector<std::string> N2EnumOptions(const ENUM_TYPE *)                              \
    {                                                                                                              \
        static const std::pair<ENUM_TYPE, nlohmann::json> n2EnumEntries[] = __VA_ARGS__;                           \
        std::vector<std::string> options;                                                                          \
        for (const auto &entry : n2EnumEntries)                                                                    \
        {                                                                                                          \
            if (entry.second.is_string())                                                                          \
            {                                                                                                      \
                const std::string &optionName = entry.second.get_ref<const std::string &>();                      \
                if (std::find(options.begin(), options.end(), optionName) == options.end())                        \
                {                                                                                                  \
                    options.push_back(optionName);                                                                 \
                }                                                                                                  \
            }                                                                                                      \
        }                                                                                                          \
        return options;                                                                                            \
    }
