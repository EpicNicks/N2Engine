#include <exception>
#include <format>
#include <utility>

#include "engine/prefabs/Prefab.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"

using namespace N2Engine;

std::string N2Engine::PrefabParseErrorToString(PrefabParseError error)
{
    switch (error)
    {
    case PrefabParseError::MissingName: return "missing 'name' field";
    case PrefabParseError::MissingRootObject: return "missing 'rootObject' field";
    case PrefabParseError::InvalidRootObject: return "failed to deserialize root object";
    case PrefabParseError::InvalidName: return "'name' is not a string";
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
    // contains() is false for anything that isn't an object, so non-object data is MissingName
    if (!j.contains("name"))
    {
        return std::unexpected(PrefabParseError::MissingName);
    }
    if (!j["name"].is_string())
    {
        return std::unexpected(PrefabParseError::InvalidName);
    }

    if (!j.contains("rootObject") || j["rootObject"].is_null())
    {
        return std::unexpected(PrefabParseError::MissingRootObject);
    }

    // GameObject::Deserialize throws on malformed data (missing keys, wrong types); that is this
    // function's InvalidRootObject, not an exception for the caller
    std::shared_ptr<GameObject> rootObject;
    try
    {
        rootObject = GameObject::Deserialize(j["rootObject"], resolver);
    }
    catch (const std::exception &e)
    {
        Logger::Error(std::format("Prefab::Deserialize: invalid root object: {}", e.what()));
        return std::unexpected(PrefabParseError::InvalidRootObject);
    }
    if (!rootObject)
    {
        return std::unexpected(PrefabParseError::InvalidRootObject);
    }

    return std::make_unique<Prefab>(j["name"].get<std::string>(), std::move(rootObject));
}
