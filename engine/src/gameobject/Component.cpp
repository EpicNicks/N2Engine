#include "engine/Component.hpp"

using namespace N2Engine;

GameObject *Component::GetGameObject() const
{
    return _gameObject.get();
}
