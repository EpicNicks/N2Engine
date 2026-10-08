#include "engine/serialization/ComponentRegistry.hpp"

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"

namespace N2Engine
{
    void ComponentRegistry::Register(const std::string &typeName, const CreateFunc &creator, const bool singleton)
    {
        const std::scoped_lock lock(_schemaMutex);
        _creators[typeName] = creator;
        if (singleton)
        {
            _singletons.insert(typeName);
        }
        else
        {
            _singletons.erase(typeName);
        }
        // A type registered again may have other fields
        _schemas.erase(typeName);
    }

    bool ComponentRegistry::IsSingleton(const std::string &typeName) const
    {
        const std::scoped_lock lock(_schemaMutex);
        return _singletons.contains(typeName);
    }

    std::optional<ComponentSchema> ComponentRegistry::Describe(const std::string &typeName)
    {
        CreateFunc creator;
        bool singleton = false;
        {
            const std::scoped_lock lock(_schemaMutex);
            if (const auto cached = _schemas.find(typeName); cached != _schemas.end())
            {
                return cached->second;
            }
            const auto found = _creators.find(typeName);
            if (found == _creators.end())
            {
                return std::nullopt;
            }
            creator = found->second;
            singleton = _singletons.contains(typeName);
        }

        ComponentSchema schema;
        schema.typeName = typeName;
        schema.singleton = singleton;
        {
            // The object is in no scene, so the component is never attached; the component goes first
            const std::shared_ptr<GameObject> holder = GameObject::Create("ComponentSchema");
            const std::unique_ptr<Component> instance = creator(*holder);
            if (instance == nullptr)
            {
                return std::nullopt;
            }
            schema.fields = instance->DescribeFields();
            schema.defaults = instance->Serialize();
        }
        // Random for every instance, and the registry has no use for it
        if (schema.defaults.is_object())
        {
            schema.defaults.erase("uuid");
            schema.defaults.erase("resourcePath");
        }

        const std::scoped_lock lock(_schemaMutex);
        _schemas[typeName] = schema;
        return schema;
    }
}
