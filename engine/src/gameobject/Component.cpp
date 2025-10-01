#include "engine/Component.hpp"
#include "engine/GameObject.hpp"

#include <cassert>

using namespace N2Engine;
using json = nlohmann::json;

GameObject &Component::GetGameObject()
{
    return _gameObject;
}

Component::Component(GameObject &gameObject)
    : Base::Asset(),
      _gameObject(gameObject),
      _isActive{true},
      _isMarkedForDestruction{false}
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

json Component::Serialize() const
{
    json j;

    // Serialize Asset base class (UUID)
    j["uuid"] = GetUUID().ToString();

    // Component-specific data
    j["isActive"] = _isActive;

    return j;
}

void Component::Deserialize(const json &j)
{
    // Deserialize Asset base (UUID)
    if (j.contains("uuid"))
    {
        _uuid = Math::UUID(j["uuid"].get<std::string>());
    }

    if (j.contains("isActive"))
    {
        _isActive = j["isActive"];
    }
}

void Component::Deserialize(const json &j, ReferenceResolver *resolver)
{
    // Default implementation just calls the old version
    // Derived classes can override to use the resolver
    Deserialize(j);
}