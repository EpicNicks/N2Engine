#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    UIGraphic::UIGraphic(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("color", _color);
        RegisterMember("raycastTarget", _raycastTarget);
    }
}
