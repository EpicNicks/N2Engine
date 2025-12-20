#include <utility>

#include "engine/prefabs/Prefab.hpp"
#include "engine/GameObject.hpp"

using namespace N2Engine;

std::string N2Engine::PrefabParseErrorToString(PrefabParseError error)
{
    switch (error)
    {
    case PrefabParseError::MissingName: return "missing 'name' field";
    case PrefabParseError::MissingRootObject: return "missing 'rootObject' field";
    case PrefabParseError::InvalidRootObject: return "failed to deserialize root object";
    }
    return "unknown error";
}

Prefab::Prefab(std::string name, std::shared_ptr<GameObject> rootObject)
    : _name(std::move(name)), _rootObject(std::move(rootObject)) {}

nlohmann::json Prefab::Serialize() const
{
    return {
        {"name", _name},
        {"rootObject", _rootObject ? _rootObject->Serialize() : nullptr}
    };
}

std::expected<std::unique_ptr<Prefab>, PrefabParseError> Prefab::Deserialize(
    const nlohmann::json &j,
    ReferenceResolver *resolver)
{
    if (!j.contains("name"))
    {
        return std::unexpected(PrefabParseError::MissingName);
    }

    if (!j.contains("rootObject") || j["rootObject"].is_null())
    {
        return std::unexpected(PrefabParseError::MissingRootObject);
    }

    auto rootObject = GameObject::Deserialize(j["rootObject"], resolver);
    if (!rootObject)
    {
        return std::unexpected(PrefabParseError::InvalidRootObject);
    }

    return std::make_unique<Prefab>(j["name"].get<std::string>(), std::move(rootObject));
}
