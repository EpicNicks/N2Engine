#include "engine/Component.hpp"

#include <cassert>

using namespace N2Engine;

GameObject &Component::GetGameObject()
{
    return _gameObject;
}

Component::Component(GameObject &gameObject) : _gameObject(gameObject), isActive{true}
{
}
