#pragma once

#include <string>
#include <vector>
#include <functional>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "engine/Component.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

namespace N2Engine
{
    using json = nlohmann::json;

    /**
     * Helper for serializing individual member variables
     */
    class MemberSerializer
    {
    public:
        using SerializeFunc = std::function<void(json &)>;
        using DeserializeFunc = std::function<void(const json &, ReferenceResolver *)>;

        std::string name;
        SerializeFunc serialize;
        DeserializeFunc deserialize;

        MemberSerializer(const std::string &name, SerializeFunc s, DeserializeFunc d)
            : name(name), serialize(s), deserialize(d) {}
    };

    /**
     * Base class for components that want automatic member serialization
     * Inherit from this instead of Component directly to get easy serialization
     */
    class SerializableComponent : public Component
    {
    protected:
        std::vector<MemberSerializer> _members;

        SerializableComponent(GameObject &gameObject) : Component(gameObject) {}

        /**
         * Register a primitive/value member for automatic serialization
         * Works with any type that nlohmann::json can handle (int, float, string, etc.)
         */
        template <typename T>
        void RegisterMember(const std::string &name, T &member)
        {
            _members.emplace_back(
                name,
                // Serialize lambda
                [&member, name](json &j)
                {
                    j[name] = member;
                },
                // Deserialize lambda
                [&member, name](const json &j, ReferenceResolver *resolver)
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
        void RegisterGameObjectRef(const std::string &name, std::shared_ptr<GameObject> &gameObjectRef);

        /**
         * Register a Component reference (resolved via UUID after deserialization)
         * Template parameter T should be the specific component type
         */
        template <typename T>
        void RegisterComponentRef(const std::string &name, std::shared_ptr<T> &componentRef)
        {
            static_assert(std::is_base_of<Component, T>::value, "T must be a Component type");

            _members.emplace_back(
                name,
                // Serialize: store UUID
                [&componentRef, name](json &j)
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
                [&componentRef, name](const json &j, ReferenceResolver *resolver)
                {
                    if (!j.contains(name) || j[name].is_null())
                    {
                        componentRef = nullptr;
                        return;
                    }

                    std::string uuidStr = j[name].get<std::string>();
                    Math::UUID uuid(uuidStr);

                    if (resolver)
                    {
                        // Add pending resolution with type casting
                        resolver->AddPendingReference([&componentRef, uuid, resolver]()
                                                      {
                            auto component = resolver->FindComponent(uuid);
                            componentRef = std::dynamic_pointer_cast<T>(component); });
                    }
                });
        }

        /**
         * Register a vector of GameObject references
         */
        void RegisterGameObjectRefVector(const std::string &name, std::vector<std::shared_ptr<GameObject>> &gameObjectRefs);

        /**
         * Register a vector of Component references
         */
        template <typename T>
        void RegisterComponentRefVector(const std::string &name, std::vector<std::shared_ptr<T>> &componentRefs)
        {
            static_assert(std::is_base_of<Component, T>::value, "T must be a Component type");

            _members.emplace_back(
                name,
                // Serialize: store array of UUIDs
                [&componentRefs, name](json &j)
                {
                    json arr = json::array();
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
                [&componentRefs, name](const json &j, ReferenceResolver *resolver)
                {
                    componentRefs.clear();

                    if (!j.contains(name))
                        return;

                    const json &arr = j[name];
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
                                componentRefs[i] = std::dynamic_pointer_cast<T>(component); });
                        }
                    }
                });
        }

    public:
        /**
         * Serialize this component and all registered members
         */
        json Serialize() const override
        {
            json j = Component::Serialize(); // Serialize base class first

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
        void Deserialize(const json &j, ReferenceResolver *resolver)
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
        void Deserialize(const json &j) override
        {
            Deserialize(j, nullptr);
        }
    };
}