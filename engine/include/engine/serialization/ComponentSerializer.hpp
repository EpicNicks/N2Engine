#pragma once

#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <functional>
#include <type_traits>
#include <nlohmann/json.hpp>

#include "engine/Component.hpp"
#include "engine/Logger.hpp"

#include "engine/serialization/FieldInfo.hpp"
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
        /// Clears the member if it points at the component (set for component references only)
        using ForgetFunc = std::function<void(const Component *)>;
        /// Clears the member if it points at the object (set for GameObject references only)
        using ForgetObjectFunc = std::function<void(const GameObject *)>;

        std::string name;
        SerializeFunc serialize;
        DeserializeFunc deserialize;
        /// What an editor shows this member as (kind, type, options, range...), filled at registration
        FieldInfo info;
        ForgetFunc forget;
        ForgetObjectFunc forgetObject;

        MemberSerializer(std::string name, SerializeFunc s, DeserializeFunc d, FieldInfo fieldInfo = {})
            : name(std::move(name)), serialize(std::move(s)), deserialize(std::move(d)), info(std::move(fieldInfo))
        {
            info.name = this->name;
            if (info.displayName.empty())
            {
                info.displayName = DefaultDisplayName(this->name);
            }
        }
    };

    /**
     * What the Register* functions return: opt-in editor metadata for the member just registered, chained on the
     * call. It holds the member's index, not a reference into the list (which the next registration may move), so it
     * stays valid however many members are registered after it; it is meant to be used straight away all the same.
     *
     *     RegisterMember(NAMEOF(_volume), _volume).Range(0.0, 1.0).Tooltip("Linear gain");
     *     RegisterMember(NAMEOF(color), color).AsColor();
     */
    class FieldBuilder
    {
    public:
        FieldBuilder(std::vector<MemberSerializer> &members, const std::size_t index)
            : _members(&members), _index(index) {}

        /// Int and Float: a value the editor sets is clamped to [lowest, highest]. (Loading a scene doesn't clamp.)
        FieldBuilder &Range(const double lowest, const double highest)
        {
            Info().range = std::make_pair(lowest, highest);
            return *this;
        }

        FieldBuilder &Tooltip(std::string text)
        {
            Info().tooltip = std::move(text);
            return *this;
        }

        /// Instead of the name made from the key ("_playOnAwake" -> "Play On Awake")
        FieldBuilder &DisplayName(std::string text)
        {
            Info().displayName = std::move(text);
            return *this;
        }

        /// A Vector3 or Vector4 member is a colour: the editor shows a picker, the JSON stays {x,y,z} (or
        /// {w,x,y,z}), so scenes saved before the hint still load. Any other type is left as it was.
        FieldBuilder &AsColor()
        {
            FieldInfo &info = Info();
            if (info.kind == FieldKind::Vector3 || info.kind == FieldKind::Vector4)
            {
                info.kind = FieldKind::Color;
            }
            return *this;
        }

        /// Shown, but the editor protocol refuses to set it
        FieldBuilder &ReadOnly()
        {
            Info().readOnly = true;
            return *this;
        }

        /// Not shown by an inspector (still saved)
        FieldBuilder &Hidden()
        {
            Info().hidden = true;
            return *this;
        }

    private:
        FieldInfo &Info() { return (*_members)[_index].info; }

        std::vector<MemberSerializer> *_members;
        std::size_t _index;
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
         * The member's type decides how an editor shows it (FieldTraits); the returned FieldBuilder refines that.
         */
        template <typename T>
        FieldBuilder RegisterMember(const std::string &name, T &member)
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
                },
                MakeFieldInfo<T>(name));
            return FieldBuilder(_members, _members.size() - 1);
        }

        /**
         * Register a GameObject reference (resolved via UUID after deserialization).
         * Takes the member by reference: it used to take the pointer by value, so the serializer
         * captured (and on load, wrote to) a copy that died when this function returned.
         */
        FieldBuilder RegisterGameObjectRef(const std::string &name, GameObject *&gameObjectRef);

        /**
         * Register a Component reference (resolved via UUID after deserialization)
         * Template parameter T should be the specific component type
         */
        // Components are owned by their GameObject (unique_ptr), so references are raw pointers.
        // (This used to take shared_ptr<T>&, which couldn't be instantiated: no shared_ptr to a
        // component exists, and the resolver hands out Component*.)
        template <typename T>
        FieldBuilder RegisterComponentRef(const std::string &name, T *&componentRef)
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
                        if (!j.contains(name))
                        {
                            Logger::Info(
                                "Component deserialize for component with name: " + name + " was not found in the json");
                        }
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
                },
                MakeReferenceInfo(name, FieldKind::ComponentRef, "Component"));
            _members.back().forget = [&componentRef](const Component *removed)
            {
                if (static_cast<const Component *>(componentRef) == removed)
                {
                    componentRef = nullptr;
                }
            };
            return FieldBuilder(_members, _members.size() - 1);
        }

        /**
         * Register a vector of GameObject references
         */
        FieldBuilder RegisterGameObjectRefVector(const std::string &name, std::vector<GameObject*> &gameObjectRefs);

        /**
         * Register a vector of Component references
         */
        template <typename T>
        FieldBuilder RegisterComponentRefVector(const std::string &name, std::vector<T *> &componentRefs)
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
                },
                MakeReferenceInfo(name, FieldKind::ComponentRefList, "Component"));
            _members.back().forget = [&componentRefs](const Component *removed)
            {
                for (T *&entry : componentRefs)
                {
                    if (static_cast<const Component *>(entry) == removed)
                    {
                        entry = nullptr;
                    }
                }
            };
            return FieldBuilder(_members, _members.size() - 1);
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
        FieldBuilder RegisterAssetRef(const std::string &name, std::shared_ptr<T> &assetRef)
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
                },
                MakeAssetInfo<T>(name, FieldKind::AssetRef));
            return FieldBuilder(_members, _members.size() - 1);
        }

        /**
         * Register a list of Asset references (a MeshRenderer's materials, one per submesh), saved as an array of
         * UUIDs where an empty slot is null. Each is resolved as RegisterAssetRef resolves one; a slot whose asset
         * isn't found (warned about) loads empty, so later slots keep their positions. A missing key or a value
         * that isn't an array (warned about) loads an empty list.
         */
        template <typename T>
        FieldBuilder RegisterAssetRefList(const std::string &name, std::vector<std::shared_ptr<T>> &assetRefs)
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
                },
                MakeAssetInfo<T>(name, FieldKind::AssetRefList));
            return FieldBuilder(_members, _members.size() - 1);
        }

    private:
        /// The FieldInfo of a reference member (a GameObject or a component, or a list of them)
        static FieldInfo MakeReferenceInfo(const std::string &name, const FieldKind kind, std::string typeName)
        {
            FieldInfo info;
            info.name = name;
            info.displayName = DefaultDisplayName(name);
            info.kind = kind;
            info.typeName = std::move(typeName);
            return info;
        }

        /// The FieldInfo of an asset reference member: its asset type is T's ResourceTypeName
        template <typename T>
        static FieldInfo MakeAssetInfo(const std::string &name, const FieldKind kind)
        {
            FieldInfo info = MakeReferenceInfo(name, kind, AssetTypeNameOf<T>());
            info.assetType = info.typeName;
            return info;
        }

    public:
        /// The registered members' FieldInfos, in registration order
        [[nodiscard]] std::vector<FieldInfo> DescribeFields() const override
        {
            std::vector<FieldInfo> fields;
            fields.reserve(_members.size());
            for (const MemberSerializer &member : _members)
            {
                fields.push_back(member.info);
            }
            return fields;
        }

        /// Drops the component references that point at `removed`
        void ForgetComponent(const Component *removed) override
        {
            for (const MemberSerializer &member : _members)
            {
                if (member.forget)
                {
                    member.forget(removed);
                }
            }
        }

        /// Drops the GameObject references that point at `removed`
        void ForgetGameObject(const GameObject *removed) override
        {
            for (const MemberSerializer &member : _members)
            {
                if (member.forgetObject)
                {
                    member.forgetObject(removed);
                }
            }
        }

        /**
         * Calls the deserialiser of exactly the members `values` has a key for, so a field it doesn't mention keeps
         * its value (the reference-list deserialisers clear their list when the key is missing, which loading a
         * whole component relies on and an edit of one field mustn't trigger). A field with a range is clamped.
         * Keys that aren't members are ignored here: the editor server rejects them first (ValidateFieldValues).
         */
        void SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver) override
        {
            if (!values.is_object())
            {
                return;
            }
            for (const MemberSerializer &member : _members)
            {
                const auto found = values.find(member.name);
                if (found == values.end())
                {
                    continue;
                }
                nlohmann::json one = nlohmann::json::object();
                one[member.name] = ClampFieldValue(member.info, *found);
                member.deserialize(one, resolver);
            }
        }

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
