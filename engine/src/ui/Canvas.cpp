#include "engine/ui/Canvas.hpp"

namespace N2Engine::UI
{
    Canvas::Canvas(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("sortOrder", _sortOrder);
    }
}
