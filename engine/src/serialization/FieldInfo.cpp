#include "engine/serialization/FieldInfo.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>

#include <math/UUID.hpp>

namespace N2Engine
{
    namespace
    {
        using json = nlohmann::json;

        /// The base keys every component's JSON has, which the editor protocol sets through its own rules
        constexpr const char *ActiveKey = "isActive";

        std::string Describe(const json &value)
        {
            return value.type_name();
        }

        std::optional<std::string> WrongType(const FieldInfo &field, std::string_view expected, const json &value)
        {
            return std::format("Field '{}': expected {}, got {}", field.name, expected, Describe(value));
        }

        /// An object with every key a number
        bool HasNumbers(const json &value, std::initializer_list<const char *> keys)
        {
            if (!value.is_object())
            {
                return false;
            }
            for (const char *key : keys)
            {
                const auto found = value.find(key);
                if (found == value.end() || !found->is_number())
                {
                    return false;
                }
            }
            return true;
        }

        /// null, or a string holding a UUID
        std::optional<std::string> CheckReference(const FieldInfo &field, const json &value)
        {
            if (value.is_null())
            {
                return std::nullopt;
            }
            if (!value.is_string())
            {
                return WrongType(field, "a UUID string or null", value);
            }
            if (!Math::UUID::FromString(value.get<std::string>()).has_value())
            {
                return std::format("Field '{}': '{}' is not a UUID", field.name, value.get<std::string>());
            }
            return std::nullopt;
        }

        /// A reference as a field with a container writes it: {"$ref": null or a UUID string}
        std::optional<std::string> CheckWrappedReference(const FieldInfo &field, const json &value)
        {
            if (!value.is_object() || !value.contains("$ref"))
            {
                return WrongType(field, "an object {\"$ref\": uuid or null}", value);
            }
            return CheckReference(field, value.at("$ref"));
        }

        /// An array whose elements are each null or a UUID string
        std::optional<std::string> CheckReferenceList(const FieldInfo &field, const json &value)
        {
            if (!value.is_array())
            {
                return WrongType(field, "an array of UUID strings or nulls", value);
            }
            for (const json &element : value)
            {
                if (auto problem = CheckReference(field, element))
                {
                    return problem;
                }
            }
            return std::nullopt;
        }
    }

    std::string_view FieldKindName(const FieldKind kind)
    {
        switch (kind)
        {
        case FieldKind::Bool: return "Bool";
        case FieldKind::Int: return "Int";
        case FieldKind::Float: return "Float";
        case FieldKind::String: return "String";
        case FieldKind::Vector2: return "Vector2";
        case FieldKind::Vector3: return "Vector3";
        case FieldKind::Vector4: return "Vector4";
        case FieldKind::Quaternion: return "Quaternion";
        case FieldKind::Color: return "Color";
        case FieldKind::Enum: return "Enum";
        case FieldKind::AssetRef: return "AssetRef";
        case FieldKind::AssetRefList: return "AssetRefList";
        case FieldKind::GameObjectRef: return "GameObjectRef";
        case FieldKind::GameObjectRefList: return "GameObjectRefList";
        case FieldKind::ComponentRef: return "ComponentRef";
        case FieldKind::ComponentRefList: return "ComponentRefList";
        case FieldKind::Json: return "Json";
        }
        return "Json";
    }

    json FieldInfo::ToJson() const
    {
        json j = {
            {"name", name},
            {"displayName", displayName},
            {"kind", std::string(FieldKindName(kind))},
            {"typeName", typeName},
            {"hidden", hidden},
            {"readOnly", readOnly},
        };
        if (!enumOptions.empty())
        {
            j["enumOptions"] = enumOptions;
        }
        if (!assetType.empty())
        {
            j["assetType"] = assetType;
        }
        if (range.has_value())
        {
            j["min"] = range->first;
            j["max"] = range->second;
        }
        if (!tooltip.empty())
        {
            j["tooltip"] = tooltip;
        }
        if (!container.empty())
        {
            j["container"] = container;
        }
        return j;
    }

    std::string DefaultDisplayName(const std::string_view name)
    {
        std::string text(name);
        // Leading underscores mark a private member
        const std::size_t first = text.find_first_not_of('_');
        text = first == std::string::npos ? std::string{} : text.substr(first);

        std::string result;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == '_')
            {
                if (!result.empty() && result.back() != ' ')
                {
                    result.push_back(' ');
                }
                continue;
            }
            const bool upper = c >= 'A' && c <= 'Z';
            if (upper && i > 0 && result.back() != ' ')
            {
                const char before = text[i - 1];
                const bool beforeLower = (before >= 'a' && before <= 'z') || (before >= '0' && before <= '9');
                const bool afterLower = i + 1 < text.size() && text[i + 1] >= 'a' && text[i + 1] <= 'z';
                // "playOnAwake": a word starts at a capital after a lower-case letter; "UIText": at the last
                // capital of a run when a lower-case letter follows
                if (beforeLower || (afterLower && before >= 'A' && before <= 'Z'))
                {
                    result.push_back(' ');
                }
            }
            result.push_back(result.empty() && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
        return result;
    }

    std::optional<std::string> ValidateFieldValue(const FieldInfo &field, const json &value)
    {
        // A reference inside a container is the wrapped form
        const bool wrapped = !field.container.empty();

        switch (field.kind)
        {
        case FieldKind::Bool:
            return value.is_boolean() ? std::nullopt : WrongType(field, "a boolean", value);
        case FieldKind::Int:
            if (value.is_number_integer())
            {
                return std::nullopt;
            }
            if (value.is_number_float() && std::isfinite(value.get<double>()) &&
                value.get<double>() == std::trunc(value.get<double>()))
            {
                return std::nullopt; // 3.0 is 3
            }
            return WrongType(field, "an integer", value);
        case FieldKind::Float:
            return value.is_number() ? std::nullopt : WrongType(field, "a number", value);
        case FieldKind::String:
            return value.is_string() ? std::nullopt : WrongType(field, "a string", value);
        case FieldKind::Vector2:
            return HasNumbers(value, {"x", "y"}) ? std::nullopt : WrongType(field, "an object {x, y} of numbers", value);
        case FieldKind::Vector3:
            return HasNumbers(value, {"x", "y", "z"}) ? std::nullopt
                                                     : WrongType(field, "an object {x, y, z} of numbers", value);
        case FieldKind::Vector4:
            return HasNumbers(value, {"w", "x", "y", "z"})
                       ? std::nullopt
                       : WrongType(field, "an object {w, x, y, z} of numbers", value);
        case FieldKind::Quaternion:
            return HasNumbers(value, {"w", "x", "y", "z"})
                       ? std::nullopt
                       : WrongType(field, "an object {w, x, y, z} of numbers", value);
        case FieldKind::Color:
            if (field.typeName == "Vector3")
            {
                return HasNumbers(value, {"x", "y", "z"}) ? std::nullopt
                                                         : WrongType(field, "a colour {x, y, z} of numbers", value);
            }
            if (field.typeName == "Vector4")
            {
                return HasNumbers(value, {"w", "x", "y", "z"})
                           ? std::nullopt
                           : WrongType(field, "a colour {w, x, y, z} of numbers", value);
            }
            if (!HasNumbers(value, {"r", "g", "b"}) || (value.contains("a") && !value.at("a").is_number()))
            {
                return WrongType(field, "a colour {r, g, b, a} of numbers", value);
            }
            return std::nullopt;
        case FieldKind::Enum:
        {
            if (!value.is_string())
            {
                return WrongType(field, "one of the options (a string)", value);
            }
            const std::string &chosen = value.get_ref<const std::string &>();
            if (std::find(field.enumOptions.begin(), field.enumOptions.end(), chosen) == field.enumOptions.end())
            {
                std::string options;
                for (const std::string &option : field.enumOptions)
                {
                    options += (options.empty() ? "" : ", ") + option;
                }
                return std::format("Field '{}': '{}' is not one of {}", field.name, chosen, options);
            }
            return std::nullopt;
        }
        case FieldKind::AssetRef:
        case FieldKind::GameObjectRef:
        case FieldKind::ComponentRef:
            return wrapped ? CheckWrappedReference(field, value) : CheckReference(field, value);
        case FieldKind::AssetRefList:
        case FieldKind::GameObjectRefList:
        case FieldKind::ComponentRefList:
            return CheckReferenceList(field, value);
        case FieldKind::Json:
            return std::nullopt;
        }
        return std::nullopt;
    }

    json ClampFieldValue(const FieldInfo &field, const json &value)
    {
        if (!field.range.has_value() || !value.is_number())
        {
            return value;
        }
        const double clamped = std::min(std::max(value.get<double>(), field.range->first), field.range->second);
        if (field.kind == FieldKind::Int)
        {
            return json(static_cast<std::int64_t>(std::llround(clamped)));
        }
        return json(clamped);
    }

    std::optional<std::string> ValidateFieldValues(const std::vector<FieldInfo> &fields, const json &values)
    {
        if (!values.is_object())
        {
            return std::format("The values must be a JSON object, got {}", Describe(values));
        }

        const auto describe = [&](const FieldInfo &field, const json &value) -> std::optional<std::string>
        {
            if (field.readOnly)
            {
                return std::format("Field '{}' is read-only", field.name);
            }
            return ValidateFieldValue(field, value);
        };

        for (const auto &[key, value] : values.items())
        {
            if (key == ActiveKey)
            {
                if (!value.is_boolean())
                {
                    return std::format("Field '{}': expected a boolean, got {}", key, Describe(value));
                }
                continue;
            }

            // A field at the top level
            const auto top = std::find_if(fields.begin(), fields.end(), [&key](const FieldInfo &field)
            {
                return field.container.empty() && field.name == key;
            });
            if (top != fields.end())
            {
                if (auto problem = describe(*top, value))
                {
                    return problem;
                }
                continue;
            }

            // Or the object holding the fields that have it as their container
            const bool isContainer = std::any_of(fields.begin(), fields.end(), [&key](const FieldInfo &field)
            {
                return field.container == key;
            });
            if (!isContainer)
            {
                return std::format("Unknown field '{}'", key);
            }
            if (!value.is_object())
            {
                return std::format("Field '{}': expected an object, got {}", key, Describe(value));
            }
            for (const auto &[innerKey, innerValue] : value.items())
            {
                const auto inner = std::find_if(fields.begin(), fields.end(), [&](const FieldInfo &field)
                {
                    return field.container == key && field.name == innerKey;
                });
                if (inner == fields.end())
                {
                    return std::format("Unknown field '{}.{}'", key, innerKey);
                }
                if (auto problem = describe(*inner, innerValue))
                {
                    return problem;
                }
            }
        }
        return std::nullopt;
    }
}
