#include "engine/prefabs/Prefab.hpp"
#include "engine/GameObject.hpp"

using namespace N2Engine;

Prefab::Prefab(const std::string &name, std::shared_ptr<GameObject> rootObject)
    : _name(name), _rootObject(rootObject)
{
}

nlohmann::json Prefab::Serialize() const
{
    nlohmann::json j;
    j["name"] = _name;
    j["rootObject"] = _rootObject->Serialize();
    return j;
}

void Prefab::Deserialize(const nlohmann::json &j)
{
    if (j.contains("name"))
    {
        _name = j["name"].get<std::string>();
    }
    if (j.contains("rootObject"))
    {
        _rootObject = GameObject::Deserialize(j["rootObject"]);
    }
}
