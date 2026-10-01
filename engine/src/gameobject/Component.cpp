#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
// ReSharper disable once CppUnusedIncludeDirective
#include "engine/serialization/MathSerialization.hpp"

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

void Component::RunDestroyCallbacks(const bool objectWasActiveInHierarchy)
{
    // Also a no-op while they run: a component that removed itself from OnDisable/OnDestroy used to start
    // the sequence again (recursing without bound) and be freed under the outer call
    if (_isMarkedForDestruction || _isRunningDestroyCallbacks)
    {
        return;
    }
    _isRunningDestroyCallbacks = true;
    // Mirrors enable: anything that got OnEnable (or started enabled) sees OnDisable before OnDestroy
    if (_isActive && objectWasActiveInHierarchy)
    {
        OnDisable();
    }
    OnDestroy();
    _isRunningDestroyCallbacks = false;
    _isMarkedForDestruction = true;
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
        _uuid = j["uuid"].get<Math::UUID>();
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