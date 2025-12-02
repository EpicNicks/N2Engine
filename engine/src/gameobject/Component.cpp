#include "engine/Component.hpp"
#include "engine/GameObject.hpp"

using namespace N2Engine;
using json = nlohmann::json;

Component::Component(GameObject &gameObject)
    : _gameObject(gameObject)
{
}

GameObject &Component::GetGameObject() const
{
    return _gameObject;
}

bool Component::IsDestroyed() const
{
    return _isMarkedForDestruction;
}

bool Component::IsActive() const
{
    return _isActive && !_isMarkedForDestruction && _gameObject.IsActiveInHierarchy();
}

void Component::SetActive(const bool active)
{
    _isActive = active;
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