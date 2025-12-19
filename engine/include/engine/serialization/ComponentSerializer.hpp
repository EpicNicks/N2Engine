#pragma once

#include <string>
#include <utility>
#include <vector>
#include <functional>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "engine/Component.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

template <typename T>
concept JsonSerializable = requires(nlohmann::json &j, const T &val, T &out)
{
    { j = val };
    { j.get<T>() } -> std::convertible_to<T>;
};

namespace N2Engine
{
    /**
     * Helper for serializing individual member variables
     */
    class MemberSerializer
    {
    public:
        using SerializeFunc = std::function<void(nlohmann::json &)>;
        using DeserializeFunc = std::function<void(const nlohmann::json &, ReferenceResolver *)>;

        std::string name;
        SerializeFunc serialize;
        DeserializeFunc deserialize;

        MemberSerializer(std::string name, SerializeFunc s, DeserializeFunc d)
            : name(std::move(name)), serialize(std::move(s)), deserialize(std::move(d)) {}
    };

    /**
     * Base class for components that want automatic member serialization
     * Inherit from this instead of Component directly to get easy serialization
     */
    class SerializableComponent : public Component
    {
    protected:
        std::vector<MemberSerializer> _members;

        explicit SerializableComponent(GameObject &gameObject) : Component(gameObject) {}

        /**
         * Register a primitive/value member for automatic serialization
         * Works with any type that nlohmann::json can handle (int, float, string, etc.)
         */
        template <typename T>
        void RegisterMember(const std::string &name, T &member)
        {
            static_assert(JsonSerializable<T>,
                          "\n"
                          "ERROR: Type is not JSON serializable!\n"
                          "...[detailed help with code examples]...\n"
            );
            _members.emplace_back(
                name,
                // Serialize lambda
                [&member, name](nlohmann::json &j)
                {
                    j[name] = member;
                },
                // Deserialize lambda
                [&member, name](const nlohmann::json &j, ReferenceResolver *resolver)
                {
                    if (j.contains(name))
                    {
                        member = j[name].get<T>();
                    }
                });
        }

        /**
         * Register a GameObject reference (resolved via UUID after deserialization)
         */
        void RegisterGameObjectRef(const std::string &name, GameObject *gameObjectRef);

        /**
         * Register a Component reference (resolved via UUID after deserialization)
         * Template parameter T should be the specific component type
         */
        template <typename T>
        void RegisterComponentRef(const std::string &name, std::shared_ptr<T> &componentRef)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must be a Component type");

            _members.emplace_back(
                name,
                // Serialize: store UUID
                [&componentRef, name](nlohmann::json &j)
                {
                    if (componentRef)
                    {
                        j[name] = componentRef->GetUUID().ToString();
                    }
                    else
                    {
                        j[name] = nullptr;
                    }
                },
                // Deserialize: schedule for later resolution
                [&componentRef, name](const nlohmann::json &j, ReferenceResolver *resolver)
                {
                    if (!j.contains(name) || j[name].is_null())
                    {
                        componentRef = nullptr;
                        Logger::Info(
                            "Component deserialize for component with name: " + name + " was not found in the json");
                        return;
                    }

                    const auto uuidStr = j[name].get<std::string>();
                    Math::UUID uuid(uuidStr);

                    if (resolver)
                    {
                        // Add pending resolution with type casting
                        resolver->AddPendingReference([&componentRef, uuid, resolver]()
                        {
                            auto component = resolver->FindComponent(uuid);
                            componentRef = std::dynamic_pointer_cast<T>(component);
                        });
                    }
                });
        }

        /**
         * Register a vector of GameObject references
         */
        void RegisterGameObjectRefVector(const std::string &name, std::vector<GameObject*> &gameObjectRefs);

        /**
         * Register a vector of Component references
         */
        template <typename T>
        void RegisterComponentRefVector(const std::string &name, std::vector<std::shared_ptr<T>> &componentRefs)
        {
            static_assert(std::is_base_of_v<Component, T>, "T must be a Component type");

            _members.emplace_back(
                name,
                // Serialize: store array of UUIDs
                [&componentRefs, name](nlohmann::json &j)
                {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto &comp : componentRefs)
                    {
                        if (comp)
                        {
                            arr.push_back(comp->GetUUID().ToString());
                        }
                        else
                        {
                            arr.push_back(nullptr);
                        }
                    }
                    j[name] = arr;
                },
                // Deserialize: schedule for later resolution
                [&componentRefs, name](const nlohmann::json &j, ReferenceResolver *resolver)
                {
                    componentRefs.clear();

                    if (!j.contains(name))
                        return;

                    const nlohmann::json &arr = j[name];
                    if (!arr.is_array())
                        return;

                    // Pre-allocate space
                    componentRefs.resize(arr.size());

                    if (resolver)
                    {
                        for (size_t i = 0; i < arr.size(); ++i)
                        {
                            if (arr[i].is_null())
                            {
                                continue;
                            }

                            std::string uuidStr = arr[i].get<std::string>();
                            Math::UUID uuid(uuidStr);

                            // Capture index for resolution
                            resolver->AddPendingReference([&componentRefs, i, uuid, resolver]()
                            {
                                auto component = resolver->FindComponent(uuid);
                                componentRefs[i] = std::dynamic_pointer_cast<T>(component);
                            });
                        }
                    }
                });
        }

    public:
        /**
         * Serialize this component and all registered members
         */
        [[nodiscard]] nlohmann::json Serialize() const override
        {
            nlohmann::json j = Component::Serialize(); // Serialize base class first

            // Serialize all registered members
            for (const auto &member : _members)
            {
                member.serialize(j);
            }

            return j;
        }

        /**
         * Deserialize this component with reference resolution support
         */
        void Deserialize(const nlohmann::json &j, ReferenceResolver *resolver) override
        {
            Component::Deserialize(j); // Deserialize base class first

            // Deserialize all registered members
            for (auto &member : _members)
            {
                member.deserialize(j, resolver);
            }
        }

        /**
         * Backwards compatibility - deserialize without resolver
         */
        void Deserialize(const nlohmann::json &j) override
        {
            Deserialize(j, nullptr);
        }
    };
}
