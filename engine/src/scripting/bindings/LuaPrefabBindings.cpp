#include "engine/scripting/LuaPrefab.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/prefabs/PrefabFile.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/JsonLimits.hpp"

// Prefabs from Lua (editor protocol 1.25.0, orchestrator/proposals/prefab-references.md): the Prefab handle a
// `type = "Prefab"` field holds, handle:Instantiate(parent?, position?, rotation?) and Prefab.Instantiate(pathOrUuid, ...)

namespace N2Engine::Scripting
{
    namespace
    {
        /// The most objects one script spawn builds (the editor's InstantiatePrefab and paste cap)
        constexpr std::size_t MaxSpawnObjects = 10000;

        const IO::AssetMetadata *MetadataOf(const Math::UUID &uuid)
        {
            return IO::ResourceLoader::Instance().GetMetadata(uuid);
        }

        /// What an error message shows of a string a script passed (cut, so a long one does not flood the log)
        std::string Shown(const std::string &text)
        {
            constexpr std::size_t MaxShown = 200;
            return text.size() > MaxShown ? text.substr(0, MaxShown) + "..." : text;
        }

        /// The prefab asset a path or UUID string names; a Lua error when there is none or it is not a prefab
        const IO::AssetMetadata &ResolvePrefab(const std::string &pathOrUuid)
        {
            const IO::AssetMetadata *meta = nullptr;
            if (const auto uuid = pathOrUuid.size() == 36 ? Math::UUID::FromString(pathOrUuid) : std::nullopt)
            {
                meta = MetadataOf(*uuid);
            }
            else if (!pathOrUuid.empty())
            {
                // With stable UUIDs a path is only a key to look the UUID up by
                meta = IO::ResourceLoader::Instance().GetMetadata(IO::ResourcePath(pathOrUuid));
            }
            if (meta == nullptr)
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: no prefab '{}'", Shown(pathOrUuid)));
            }
            if (meta->resourceType != PrefabFile::ResourceTypeName)
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: '{}' is a {}, not a Prefab", Shown(pathOrUuid),
                                                     meta->resourceType.empty() ? std::string("file") : meta->resourceType));
            }
            return *meta;
        }

        /// What one walk over a prefab's root object finds, before anything is built
        struct PrefabScan
        {
            std::size_t objects = 0;
            std::size_t levels = 0;
            bool hasNetworkObject = false;
        };

        /// Counted with a stack on the heap (prefab data can nest as deep as the scene allows)
        PrefabScan Scan(const nlohmann::json &rootObject)
        {
            PrefabScan scan;
            std::vector<std::pair<const nlohmann::json *, std::size_t>> pending{{&rootObject, 1}};
            while (!pending.empty())
            {
                const auto [object, level] = pending.back();
                pending.pop_back();
                if (!object->is_object())
                {
                    continue;
                }
                ++scan.objects;
                scan.levels = std::max(scan.levels, level);
                if (const auto components = object->find("components");
                    components != object->end() && components->is_array())
                {
                    for (const nlohmann::json &component : *components)
                    {
                        if (component.is_object() && component.value("type", nlohmann::json()) == "NetworkObject")
                        {
                            scan.hasNetworkObject = true;
                        }
                    }
                }
                if (const auto children = object->find("children"); children != object->end() && children->is_array())
                {
                    for (const nlohmann::json &child : *children)
                    {
                        pending.emplace_back(&child, level + 1);
                    }
                }
            }
            return scan;
        }

        /// One warning per prefab per process when an instance came out with references cleared
        void WarnClearedOnce(const IO::AssetMetadata &meta, const std::size_t cleared)
        {
            static std::unordered_set<std::string> warned; // game thread only, as every Lua call
            if (cleared == 0 || !warned.insert(meta.uuid.ToString()).second)
            {
                return;
            }
            Logger::Warn(std::format("Prefab {}: {} reference(s) to objects outside the prefab were cleared when a "
                                     "script instantiated it (a prefab file cannot keep them: find them from the "
                                     "instance's script or assign them on the returned object). Said once per prefab.",
                                     meta.resourcePath.ToString(), cleared));
        }

        bool IsFinite(const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /// The optional argument at `index`: absent or nil is none; anything else must be a T
        template <typename T>
        std::optional<T> Optional(const sol::variadic_args &args, const std::size_t index, const char *what)
        {
            if (index >= args.size())
            {
                return std::nullopt;
            }
            const sol::stack_proxy argument = args[index];
            if (argument.get_type() == sol::type::lua_nil || argument.get_type() == sol::type::none)
            {
                return std::nullopt;
            }
            if (!argument.is<T>())
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: argument {} must be {} or nil", index + 1, what));
            }
            return argument.get<T>();
        }

        /// The spawn itself: checks, builds, places and returns the instance's root
        GameObjectRef Spawn(const IO::AssetMetadata &meta, const std::optional<GameObjectRef> &parentRef,
                            const std::optional<Math::Vector3> &position, const std::optional<Math::Quaternion> &rotation)
        {
            const std::string shown = meta.resourcePath.ToString();
            if (position && !IsFinite(*position))
            {
                throw std::runtime_error("Prefab.Instantiate: the position has a value that isn't a finite number");
            }
            if (rotation && !(std::isfinite(rotation->GetW()) && std::isfinite(rotation->GetX()) &&
                              std::isfinite(rotation->GetY()) && std::isfinite(rotation->GetZ())))
            {
                throw std::runtime_error("Prefab.Instantiate: the rotation has a value that isn't a finite number");
            }

            const GameObject::Ptr parent = parentRef ? parentRef->Pin() : nullptr;
            Scene *scene = nullptr;
            if (!parent)
            {
                scene = SceneManager::GetCurScene();
                if (scene == nullptr)
                {
                    throw std::runtime_error("Prefab.Instantiate: no scene is loaded to add the instance to");
                }
            }

            const auto prefab = IO::ResourceLoader::Instance().LoadByUUID<PrefabFile>(meta.uuid);
            if (!prefab)
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: can't load prefab '{}' (see the log)", shown));
            }
            const nlohmann::json &data = prefab->GetData();
            const auto root = data.find("rootObject");
            if (root == data.end() || !root->is_object())
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: prefab '{}' has no root object", shown));
            }

            // Everything is checked before anything is built, so a refused spawn adds nothing
            const PrefabScan scan = Scan(*root);
            if (scan.objects > MaxSpawnObjects)
            {
                throw std::runtime_error(
                    std::format("Prefab.Instantiate: prefab '{}' has more than {} objects", shown, MaxSpawnObjects));
            }
            const std::size_t levelsAbove = parent ? parent->GetHierarchyDepth() + 1 : 0;
            if (levelsAbove + scan.levels > JsonLimits::MaxHierarchyDepth)
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: prefab '{}' would nest deeper than the {} "
                                                     "levels a scene may have",
                                                     shown, JsonLimits::MaxHierarchyDepth));
            }
            // A local copy of a networked object would not replicate, have no net id, and on a client diverge: refused
            // while a session runs (replicating spawns by prefab UUID is a later stage)
            if (scan.hasNetworkObject &&
                (Network::NetworkServer::Active() != nullptr || Network::NetworkClient::Active() != nullptr))
            {
                throw std::runtime_error(std::format(
                    "Prefab.Instantiate: prefab '{}' has a NetworkObject: use Network.Spawn (not available yet)", shown));
            }

            // Fresh UUIDs, references inside the prefab point at the copies, every other reference is cleared (no
            // resolver for the running scene: a stale UUID there could bind the wrong object)
            std::size_t cleared = 0;
            std::vector<GameObject::Ptr> roots = PrefabManager::InstantiatePrefabs({&*root}, nullptr, &cleared);
            if (roots.empty() || !roots.front())
            {
                throw std::runtime_error(std::format("Prefab.Instantiate: can't instantiate '{}' (see the log)", shown));
            }
            const GameObject::Ptr instance = roots.front();
            instance->SetPrefabReference(meta.uuid);

            if (parent)
            {
                parent->AddChild(instance, false);
                if (instance->GetParent() != parent)
                {
                    throw std::runtime_error(
                        std::format("Prefab.Instantiate: the instance of '{}' could not be added to the parent", shown));
                }
            }
            else
            {
                scene->AddRootGameObject(instance);
            }

            // World values, once the parent is known; an object without a transform gets one to be placed with
            if (position || rotation)
            {
                if (instance->GetPositionable() == nullptr)
                {
                    instance->CreatePositionable();
                }
                if (position)
                {
                    instance->GetPositionable()->SetPosition(*position);
                }
                if (rotation)
                {
                    instance->GetPositionable()->SetRotation(*rotation);
                }
            }

            WarnClearedOnce(meta, cleared);
            return GameObjectRef(instance); // the parent or the scene owns it
        }

        /// handle:Instantiate(parent?, position?, rotation?) and Prefab.Instantiate(pathOrUuid, parent?, position?, rotation?)
        GameObjectRef Instantiate(const sol::variadic_args &args)
        {
            if (args.size() == 0)
            {
                throw std::runtime_error("Prefab.Instantiate: expected a Prefab, a res:// path or a UUID");
            }
            const sol::stack_proxy target = args[0];
            const IO::AssetMetadata *meta = nullptr;
            if (target.is<PrefabHandle>())
            {
                const PrefabHandle handle = target.get<PrefabHandle>();
                meta = &ResolvePrefab(handle.GetUUID().ToString());
            }
            else if (target.get_type() == sol::type::string)
            {
                meta = &ResolvePrefab(target.get<std::string>());
            }
            else
            {
                throw std::runtime_error("Prefab.Instantiate: expected a Prefab, a res:// path or a UUID");
            }
            const std::optional<GameObjectRef> parent = Optional<GameObjectRef>(args, 1, "a GameObject");
            const std::optional<Math::Vector3> position = Optional<Math::Vector3>(args, 2, "a Vector3");
            const std::optional<Math::Quaternion> rotation = Optional<Math::Quaternion>(args, 3, "a Quaternion");
            // Copied: the spawn loads the prefab's scripts, and the index the record lives in must not be relied on meanwhile
            const IO::AssetMetadata record = *meta;
            return Spawn(record, parent, position, rotation);
        }
    }

    bool PrefabHandle::IsValid() const
    {
        const IO::AssetMetadata *meta = MetadataOf(_uuid);
        return meta != nullptr && meta->resourceType == PrefabFile::ResourceTypeName;
    }

    std::string PrefabHandle::GetPath() const
    {
        const IO::AssetMetadata *meta = MetadataOf(_uuid);
        return meta != nullptr ? meta->resourcePath.ToString() : std::string{};
    }

    std::string PrefabHandle::GetName() const
    {
        const IO::AssetMetadata *meta = MetadataOf(_uuid);
        return meta != nullptr ? meta->resourcePath.GetStem() : std::string{};
    }

    namespace Bindings
    {
        void BindPrefab(LuaRuntime &runtime)
        {
            auto &lua = runtime.GetState();
            lua.new_usertype<PrefabHandle>(
                "Prefab",
                sol::no_constructor,
                sol::meta_function::equal_to, [](const PrefabHandle &a, const PrefabHandle &b) { return a == b; },
                sol::meta_function::to_string, [](const PrefabHandle &handle)
                {
                    const std::string path = handle.GetPath();
                    return std::format("Prefab({})", path.empty() ? handle.GetUUID().ToString() + ", missing" : path);
                },
                "valid", sol::readonly_property([](const PrefabHandle &handle) { return handle.IsValid(); }),
                "uuid", sol::readonly_property([](const PrefabHandle &handle) { return handle.GetUUID().ToString(); }),
                "path", sol::readonly_property([](const PrefabHandle &handle) { return handle.GetPath(); }),
                "name", sol::readonly_property([](const PrefabHandle &handle) { return handle.GetName(); }),
                "Instantiate", [](const sol::variadic_args args) { return Instantiate(args); });
        }
    }
}
