#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace N2Engine
{
    class GameObject;

    class Prefab
    {
    private:
        std::string _name;
        std::shared_ptr<GameObject> _rootObject;

    public:
        Prefab(const std::string &name, std::shared_ptr<GameObject> rootObject);

        const std::string &GetName() const { return _name; }
        std::shared_ptr<GameObject> GetRootObject() const { return _rootObject; }

        nlohmann::json Serialize() const;
        void Deserialize(const nlohmann::json &j);
    };
}