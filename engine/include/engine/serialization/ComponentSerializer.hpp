#pragma once

#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <functional>
#include <type_traits>
#include <nlohmann/json.hpp>

#include "engine/Component.hpp"
#include "engine/Logger.hpp"

#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/io/Resources.hpp"
#include "engine/io/ResourceLoader.hpp"
// ReSharper disable once CppUnusedIncludeDirective
#include "engine/serialization/MathSerialization.hpp"

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
         * Register a GameObject reference (resolved via UUID after deserialization).
         * Takes the member by reference: it used to take the pointer by value, so the serializer
         * captured (and on load, wrote to) a copy that died when this function returned.
         */
        void RegisterGameObjectRef(const std::string &name, GameObject *&gameObjectRef);

        /**
         * Register a Component reference (resolved via UUID after deserialization)
         * Template parameter T should be the specific component type
         */
        // Components are owned by their GameObject (unique_ptr), so references are raw pointers.
        // (This used to take shared_ptr<T>&, which couldn't be instantiated: no shared_ptr to a
        // component exists, and the resolver hands out Component*.)
        template <typename T>
        void RegisterComponentRef(const std::string &name, T *&componentRef)
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

                    const auto uuid = j[name].get<Math::UUID>();

                    if (resolver)
                    {
                        // Add pending resolution with type casting
                        resolver->AddPendingReference([&componentRef, uuid, resolver]()
                        {
                            componentRef = dynamic_cast<T *>(resolver->FindComponent(uuid));
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
        void RegisterComponentRefVector(const std::string &name, std::vector<T *> &componentRefs)
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

                            const auto uuid = arr[i].get<Math::UUID>();

                            // Capture index for resolution
                            resolver->AddPendingReference([&componentRefs, i, uuid, resolver]()
                            {
                                componentRefs[i] = dynamic_cast<T *>(resolver->FindComponent(uuid));
                            });
                        }
                    }
                });
        }

        /**
         * The asset a saved reference names: null for a JSON null, else the asset with that UUID. An asset type
         * with built-in assets (a static FindBuiltin(UUID), such as Rendering::Mesh) is checked first, so its
         * built-ins resolve without being registered anywhere; then Resources (project assets by their stable
         * .meta UUIDs, and assets registered at runtime this run). nullptr, with a warning naming `name`, for an
         * invalid UUID or an asset that isn't found.
         */
        template <typename T>
        static std::shared_ptr<T> ResolveAssetReference(const std::string &name, const nlohmann::json &value)
        {
            if (value.is_null())
            {
                return nullptr;
            }
            const auto uuid = value.get<Math::UUID>();
            if (uuid == Math::UUID::ZERO)
            {
                Logger::Warn(std::format("Invalid UUID for asset '{}': {}", name, uuid.ToString()));
                return nullptr;
            }
            if constexpr (requires(const Math::UUID &id) { { T::FindBuiltin(id) } -> std::convertible_to<std::shared_ptr<T>>; })
            {
                if (std::shared_ptr<T> builtin = T::FindBuiltin(uuid))
                {
                    return builtin;
                }
            }
            // Project assets are found (or loaded) by their stable .meta UUIDs; assets registered at runtime
            // have random per-process UUIDs, so only this run finds those
            std::shared_ptr<T> asset = IO::Resources::Instance().LoadByUUID<T>(uuid);
            if (!asset)
            {
                Logger::Warn(std::format("Asset '{}' not found: {}", name, uuid.ToString()));
            }
            return asset;
        }

        /**
 * Register an Asset reference (resolved via UUID after deserialization)
 * Template parameter T should be the specific asset type
 */
        template <typename T>
        void RegisterAssetRef(const std::string &name, std::shared_ptr<T> &assetRef)
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            _members.emplace_back(
                name,
                // Serialize: store UUID
                [&assetRef, name](nlohmann::json &j)
                {
                    if (assetRef)
                    {
                        j[name] = assetRef->GetUUID().ToString();
                    }
                    else
                    {
                        j[name] = nullptr;
                    }
                },
                // Deserialize: resolve from AssetManager
                [&assetRef, name](const nlohmann::json &j, ReferenceResolver *)
                {
                    if (!j.contains(name))
                    {
                        assetRef = nullptr;
                        return;
                    }
                    assetRef = ResolveAssetReference<T>(name, j[name]);
                });
        }

        /**
         * Register a list of Asset references (a MeshRenderer's materials, one per submesh), saved as an array of
         * UUIDs where an empty slot is null. Each is resolved as RegisterAssetRef resolves one; a slot whose asset
         * isn't found (warned about) loads empty, so later slots keep their positions. A missing key or a value
         * that isn't an array (warned about) loads an empty list.
         */
        template <typename T>
        void RegisterAssetRefList(const std::string &name, std::vector<std::shared_ptr<T>> &assetRefs)
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            _members.emplace_back(
                name,
                [&assetRefs, name](nlohmann::json &j)
                {
                    nlohmann::json array = nlohmann::json::array();
                    for (const auto &asset : assetRefs)
                    {
                        if (asset)
                        {
                            array.push_back(asset->GetUUID().ToString());
                        }
                        else
                        {
                            array.push_back(nullptr);
                        }
                    }
                    j[name] = std::move(array);
                },
                [&assetRefs, name](const nlohmann::json &j, ReferenceResolver *)
                {
                    assetRefs.clear();
                    if (!j.contains(name) || j[name].is_null())
                    {
                        return;
                    }
                    const nlohmann::json &array = j[name];
                    if (!array.is_array())
                    {
                        Logger::Warn(std::format("Asset list '{}' isn't an array: {}", name, array.dump()));
                        return;
                    }
                    assetRefs.reserve(array.size());
                    for (std::size_t i = 0; i < array.size(); ++i)
                    {
                        assetRefs.push_back(ResolveAssetReference<T>(std::format("{}[{}]", name, i), array[i]));
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
