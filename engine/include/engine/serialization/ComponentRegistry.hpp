#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

namespace N2Engine
{
    class Component;
    class GameObject;

    /**
     * Central registry for component types
     * Allows creation of components by type name string
     */
    class ComponentRegistry
    {
    public:
        using CreateFunc = std::function<std::shared_ptr<Component>(GameObject &)>;

    private:
        std::unordered_map<std::string, CreateFunc> _creators;

        ComponentRegistry() = default;

    public:
        /**
         * Get singleton instance
         */
        static ComponentRegistry &Instance()
        {
            static ComponentRegistry instance;
            return instance;
        }

        // Delete copy/move constructors
        ComponentRegistry(const ComponentRegistry &) = delete;
        ComponentRegistry &operator=(const ComponentRegistry &) = delete;

        /**
         * Register a component type with a creation function
         */
        void Register(const std::string &typeName, CreateFunc creator)
        {
            _creators[typeName] = creator;
        }

        /**
         * Create a component by type name
         * Returns nullptr if type is not registered
         */
        std::shared_ptr<Component> Create(const std::string &typeName, GameObject &gameObject)
        {
            auto it = _creators.find(typeName);
            if (it != _creators.end())
            {
                return it->second(gameObject);
            }
            return nullptr;
        }

        /**
         * Check if a type is registered
         */
        bool IsRegistered(const std::string &typeName) const
        {
            return _creators.find(typeName) != _creators.end();
        }

        /**
         * Get all registered type names
         */
        std::vector<std::string> GetRegisteredTypes() const
        {
            std::vector<std::string> types;
            types.reserve(_creators.size());
            for (const auto &pair : _creators)
            {
                types.push_back(pair.first);
            }
            return types;
        }
    };

    /**
     * Auto-registration helper class
     * Create a static instance of this to auto-register a component type
     */
    template <typename T>
    class ComponentRegistrar
    {
    public:
        explicit ComponentRegistrar(const std::string &typeName)
        {
            ComponentRegistry::Instance().Register(
                typeName,
                [](GameObject &go) -> std::shared_ptr<Component>
                {
                    return std::make_shared<T>(go);
                });
        }
    };
}

/**
 * Macro for easy component registration
 * Place this in your component's .cpp file or at the end of the header
 *
 * Example:
 *   REGISTER_COMPONENT(MyCustomComponent)
 */
#define REGISTER_COMPONENT(ClassName) \
    static N2Engine::ComponentRegistrar<ClassName> _registrar_##ClassName(#ClassName);
