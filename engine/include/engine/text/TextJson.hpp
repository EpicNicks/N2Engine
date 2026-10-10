#pragma once

#include <nlohmann/json.hpp>
#include <text/TextLayout.hpp>

#include "engine/serialization/FieldInfo.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include "engine/text/TextEffects.hpp"

// JSON names for the text library's layout enums, used by components that save them (the text library
// itself doesn't depend on nlohmann). An unknown name reads as the first entry (Left / Top). Also the JSON of
// an extra effect pass (TextPass), which TextRenderer and UI::UIText save as a list.
namespace N2Engine::Text
{
    /// {"color", "offset", "width", "softness", "order"}
    inline void to_json(nlohmann::json &j, const TextPass &pass)
    {
        j = nlohmann::json{{"color", pass.color},
                           {"offset", pass.offset},
                           {"width", pass.width},
                           {"softness", pass.softness},
                           {"order", pass.order}};
    }

    /// A key that is missing keeps the TextPass default; anything that isn't an object is a default pass
    inline void from_json(const nlohmann::json &j, TextPass &pass)
    {
        pass = TextPass{};
        if (!j.is_object())
        {
            return;
        }
        if (j.contains("color"))
        {
            pass.color = j["color"].get<Common::Color>();
        }
        if (j.contains("offset"))
        {
            pass.offset = j["offset"].get<Math::Vector2>();
        }
        pass.width = j.value("width", pass.width);
        pass.softness = j.value("softness", pass.softness);
        pass.order = j.value("order", pass.order);
    }

    N2_SERIALIZE_ENUM(HorizontalAlign, {
                      { HorizontalAlign::Left, "Left" },
                      { HorizontalAlign::Center, "Center" },
                      { HorizontalAlign::Right, "Right" }
                      })

    N2_SERIALIZE_ENUM(VerticalAlign, {
                      { VerticalAlign::Top, "Top" },
                      { VerticalAlign::Middle, "Middle" },
                      { VerticalAlign::Bottom, "Bottom" },
                      { VerticalAlign::Baseline, "Baseline" }
                      })
}

namespace N2Engine
{
    /// The extra effect passes of a text: still the Json kind (a list of TextPass objects, above), but the type
    /// name tells an editor to draw a pass list editor instead of a generic JSON box
    template <>
    struct FieldTraits<std::vector<Text::TextPass>>
    {
        static constexpr FieldKind kind = FieldKind::Json;
        static std::string TypeName() { return "TextPass[]"; }
        static std::vector<std::string> EnumOptions() { return {}; }
    };
}
