#pragma once

#include <nlohmann/json.hpp>
#include <text/TextLayout.hpp>

#include "engine/serialization/FieldInfo.hpp"

// JSON names for the text library's layout enums, used by components that save them (the text library
// itself doesn't depend on nlohmann). An unknown name reads as the first entry (Left / Top).
namespace N2Engine::Text
{
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
