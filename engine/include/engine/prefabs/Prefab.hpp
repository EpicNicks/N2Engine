#pragma once

#include <expected>
#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace N2Engine
{
    class GameObject;
    class ReferenceResolver;

    enum class PrefabParseError
    {
        MissingName,
        MissingRootObject,
        InvalidRootObject
    };

    std::string PrefabParseErrorToString(PrefabParseError error);

    class Prefab
    {
    private:
        std::string _name;
        std::shared_ptr<GameObject> _rootObject;

    public:
        Prefab(std::string name, std::shared_ptr<GameObject> rootObject);

        [[nodiscard]] const std::string& GetName() const { return _name; }
        [[nodiscard]] std::shared_ptr<GameObject> GetRootObject() const { return _rootObject; }

        [[nodiscard]] nlohmann::json Serialize() const;
        static std::expected<std::unique_ptr<Prefab>, PrefabParseError> Deserialize(
            const nlohmann::json &j,
            ReferenceResolver *resolver = nullptr
        );
    };
}
