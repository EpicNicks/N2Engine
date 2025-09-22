#include "engine/Component.hpp"
#include "engine/GameObject.hpp"

#include <cassert>

using namespace N2Engine;

GameObject &Component::GetGameObject()
{
    return _gameObject;
}

Component::Component(GameObject &gameObject) : _gameObject(gameObject), _isActive{true}, _isMarkedForDestruction{false}
{
}

bool Component::IsDestroyed()
{
    return _isMarkedForDestruction;
}

bool Component::GetIsActive()
{
    return _isActive && !_isMarkedForDestruction && _gameObject.IsActiveInHierarchy();
}
