#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <ranges>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;
    class ComponentRegistry;

    /// Registers the engine's own component types (BuiltinComponents.cpp). The registry's constructor calls
    /// it, so using the registry links them in.
    void RegisterBuiltinComponents(ComponentRegistry &registry);

    /// What an editor needs to know about a component type without an instance of it in a scene
    struct ComponentSchema
    {
        std::string typeName;
        /// The editable fields (Component::DescribeFields of a throwaway instance)
        std::vector<FieldInfo> fields;
        /// What a new component of the type saves (its Serialize(), without the random uuid), isActive included.
        /// Null in the schema of a live component (see EditorServer's GetLuaFields).
        nlohmann::json defaults;
        /// At most one per object (the type's IsSingleton)
        bool singleton = false;

        /// The ComponentSchema of the editor protocol (protocol.json)
        [[nodiscard]] nlohmann::json ToJson() const
        {
            nlohmann::json j = {{"typeName", typeName}, {"singleton", singleton}, {"fields", nlohmann::json::array()}};
            for (const FieldInfo &field : fields)
            {
                j["fields"].push_back(field.ToJson());
            }
            if (!defaults.is_null())
            {
                j["defaults"] = defaults;
            }
            return j;
        }
    };

    /**
     * Central registry for component types
     * Allows creation of components by type name string
     */
    class ComponentRegistry
    {
    public:
        using CreateFunc = std::function<std::unique_ptr<Component>(GameObject &)>;

    private:
        std::unordered_map<std::string, CreateFunc> _creators;
        std::unordered_set<std::string> _singletons;
        // Describe's results: a type's schema doesn't change while it stays registered
        std::unordered_map<std::string, ComponentSchema> _schemas;
        mutable std::mutex _schemaMutex;

        ComponentRegistry() { RegisterBuiltinComponents(*this); }

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
         * Register a component type with a creation function. `singleton`: an object can have only one (the
         * editor server refuses a second AddComponent); registering a name again replaces the type.
         */
        void Register(const std::string &typeName, const CreateFunc& creator, bool singleton = false);

        /**
         * What an editor shows for a type, or nullopt for a name that isn't registered. Made from a throwaway
         * instance on a detached object (in no scene, so it is never attached: no physics body, audio source
         * or GPU resource is made), and cached.
         */
        [[nodiscard]] std::optional<ComponentSchema> Describe(const std::string &typeName);

        /// Whether the type is registered as a singleton (an object can have only one)
        [[nodiscard]] bool IsSingleton(const std::string &typeName) const;

        /**
         * Create a component by type name
         * Returns nullptr if type is not registered
         */
        std::unique_ptr<Component> Create(const std::string &typeName, GameObject &gameObject)
        {
            if (const auto it = _creators.find(typeName); it != _creators.end())
            {
                return it->second(gameObject);
            }
            return nullptr;
        }

        /**
         * Check if a type is registered
         */
        [[nodiscard]] bool IsRegistered(const std::string &typeName) const
        {
            return _creators.contains(typeName);
        }

        /**
         * Get all registered type names
         */
        [[nodiscard]] std::vector<std::string> GetRegisteredTypes() const
        {
            std::vector<std::string> types;
            types.reserve(_creators.size());
            for (const auto& key : _creators | std::views::keys)
            {
                types.push_back(key);
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
                [](GameObject &go) -> std::unique_ptr<Component>
                {
                    return std::make_unique<T>(go);
                },
                T::IsSingleton);
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
