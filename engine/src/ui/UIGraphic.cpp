#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    UIGraphic::UIGraphic(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("color", _color);
        RegisterMember("raycastTarget", _raycastTarget);
    }

    Common::Color UIGraphic::GetDrawColor() const
    {
        return Common::Color{_color.r * _tint.r, _color.g * _tint.g, _color.b * _tint.b, _color.a * _tint.a};
    }
}
