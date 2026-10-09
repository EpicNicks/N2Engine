#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <future>
#include <system_error>
#include <utility>

#include "engine/Application.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/Version.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/picking/ScenePicking.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/scripting/LuaScriptTemplate.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

#include "renderer/common/FrameRows.hpp"
#include "renderer/common/Renderer.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/Protocol.hpp"
#include "editor-server/Commands.hpp"
#include "editor-server/Serialization.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
using NativeSocket = SOCKET;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#define CLOSE_SOCKET close
#define INVALID_SOCKET -1
#define SOCKET_ERROR_CODE errno
using NativeSocket = int;
#endif

namespace N2Engine::Editor
{
    using namespace Protocol;

    namespace
    {
        // Sockets are kept as int (as before); an invalid SOCKET converts to -1
        constexpr int NoSocket = -1;
        // How often a waiting network thread checks whether the server is stopping
        constexpr long PollIntervalMicroseconds = 100 * 1000;

        /// A path as UTF-8 text for the wire (string() would throw for characters the code page lacks)
        std::string Utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        /// A UTF-8 path from the wire as a filesystem path
        std::filesystem::path FromUtf8(const std::string &text)
        {
            return std::filesystem::path(std::u8string(text.begin(), text.end()));
        }

        std::vector<std::string> PathStrings(const std::vector<IO::ResourcePath> &paths)
        {
            std::vector<std::string> strings;
            strings.reserve(paths.size());
            for (const IO::ResourcePath &path : paths)
            {
                strings.push_back(path.ToString());
            }
            return strings;
        }

        std::expected<std::string, std::string> ReadTextFile(const std::filesystem::path &file)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return std::unexpected("can't read " + Utf8(file));
            }
            std::ostringstream text;
            text << stream.rdbuf();
            if (stream.bad())
            {
                return std::unexpected("can't read " + Utf8(file));
            }
            return std::move(text).str();
        }

        /// A scene's JSON as its file holds it: indented by 2, ending in a newline (diffs well under version control)
        std::string SceneFileText(const nlohmann::json &scene)
        {
            return scene.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        }

        // ==================== Assets ====================

        /// The extensions ReadTextAsset and WriteTextAsset handle (see EditorServer::IsTextAssetPath), lower case
        constexpr std::array<std::string_view, 17> TextAssetExtensions = {
            ".lua", ".mat", ".scene", ".json", ".txt", ".md", ".csv", ".xml", ".yaml",
            ".yml", ".toml", ".ini", ".cfg", ".glsl", ".vert", ".frag", ".shader"};

        std::string LowerAscii(const std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        /// ListAssets's AssetInfo for an indexed asset (see protocol.json): its path, UUID, type, size, time and sub-assets
        nlohmann::json AssetInfoJson(const IO::AssetMetadata &meta)
        {
            nlohmann::json info{{"path", meta.resourcePath.ToString()},
                                {"uuid", meta.uuid.ToString()},
                                {"type", meta.resourceType},
                                {"size", meta.fileSize},
                                {"modified", meta.lastModified}};
            const std::vector<IO::ResourceLoader::SubAssetIndexEntry> subAssets =
                IO::ResourceLoader::Instance().GetSubAssets(meta.resourcePath);
            if (!subAssets.empty())
            {
                nlohmann::json list = nlohmann::json::array();
                for (const IO::ResourceLoader::SubAssetIndexEntry &entry : subAssets)
                {
                    list.push_back(nlohmann::json{{"key", entry.key}, {"uuid", entry.uuid.ToString()}, {"type", entry.type}});
                }
                info["subAssets"] = std::move(list);
            }
            return info;
        }

        // ==================== Hierarchy and entities ====================

        nlohmann::json Vec3Json(const Math::Vector3 &v)
        {
            return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
        }

        nlohmann::json QuatJson(const Math::Quaternion &q)
        {
            return nlohmann::json{{"x", q.GetX()}, {"y", q.GetY()}, {"z", q.GetZ()}, {"w", q.GetW()}};
        }

        /// The object with this UUID string in the scene, or nullptr (also for text that isn't a UUID)
        std::shared_ptr<GameObject> FindEntity(Scene &scene, const std::string &entityId)
        {
            if (const auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
            {
                return scene.FindGameObjectByUUID(uuid.value());
            }
            return nullptr;
        }

        std::string NotFoundMessage(const char *what, const std::string &entityId)
        {
            return std::format("{} not found: {}", what, EditorServer::SanitizeForLog(entityId));
        }

        /// The UUID string of an object's parent, or empty for a root
        std::string ParentIdOf(const GameObject &gameObject)
        {
            const auto parent = gameObject.GetParent();
            return parent ? parent->GetUUID().ToString() : std::string{};
        }

        /// The fields GetHierarchy and GetEntity share (the EntityHeader of protocol.json, plus what a node adds)
        nlohmann::json HeaderJson(const GameObject &gameObject, const std::string &parentId, const size_t index)
        {
            return nlohmann::json{
                {"id", gameObject.GetUUID().ToString()},
                {"parentId", parentId},
                {"index", index},
                {"name", gameObject.GetName()},
                {"active", gameObject.IsActive()},
                {"activeInHierarchy", gameObject.IsActiveInHierarchy()},
                {"layer", gameObject.GetLayer()},
                {"tag", gameObject.GetTag()},
            };
        }

        /// Appends the object and, depth first, everything under it: a HierarchyNode each
        void AppendHierarchy(const GameObject &gameObject, const std::string &parentId, const size_t index,
                             nlohmann::json &nodes)
        {
            nlohmann::json node = HeaderJson(gameObject, parentId, index);
            nlohmann::json components = nlohmann::json::array();
            for (const auto &component : gameObject.GetAllComponents())
            {
                components.push_back(component->GetTypeName());
            }
            node["components"] = std::move(components);
            nodes.push_back(std::move(node));

            const std::string id = gameObject.GetUUID().ToString();
            size_t childIndex = 0;
            for (const auto &child : gameObject.GetChildren())
            {
                if (child && !child->IsDestroyed())
                {
                    AppendHierarchy(*child, id, childIndex, nodes);
                }
                ++childIndex;
            }
        }

        /// The EntityDetails of protocol.json: header, local transform (when the object has one) and the components
        /// as they are saved
        nlohmann::json EntityDetailsJson(const GameObject &gameObject)
        {
            nlohmann::json entity;
            entity["header"] = HeaderJson(gameObject, ParentIdOf(gameObject), gameObject.GetSiblingIndex());
            if (const Positionable *positionable = gameObject.GetPositionable())
            {
                entity["transform"] = nlohmann::json{
                    {"position", Vec3Json(positionable->GetLocalPosition())},
                    {"rotation", QuatJson(positionable->GetLocalRotation())},
                    {"scale", Vec3Json(positionable->GetLocalScale())},
                };
            }
            nlohmann::json components = nlohmann::json::array();
            for (const auto &component : gameObject.GetAllComponents())
            {
                components.push_back(nlohmann::json{
                    {"type", component->GetTypeName()},
                    {"uuid", component->GetUUID().ToString()},
                    {"values", component->Serialize()},
                });
            }
            entity["components"] = std::move(components);
            return entity;
        }

        /// The UUID strings of the object and everything under it
        std::vector<std::string> SubtreeIds(const GameObject &gameObject)
        {
            std::vector<std::string> ids{gameObject.GetUUID().ToString()};
            for (const auto &descendant : gameObject.GetChildrenRecursive())
            {
                ids.push_back(descendant->GetUUID().ToString());
            }
            return ids;
        }

        /// What CreateEntityEx's preset adds to the new object (which always gets a transform)
        struct EntityPreset
        {
            std::string_view name;
            /// The new object's name when the request gives none
            std::string_view objectName;
            void (*addComponents)(GameObject &);
        };

        const std::vector<EntityPreset> &EntityPresets()
        {
            static const std::vector<EntityPreset> presets = {
                {"", "GameObject", [](GameObject &) {}},
                {"Empty", "GameObject", [](GameObject &) {}},
                {"Cube", "Cube", [](GameObject &object) { object.AddComponent<Example::CubeRenderer>(); }},
                {"Sphere", "Sphere", [](GameObject &object) { object.AddComponent<Example::SphereRenderer>(); }},
                {"Quad", "Quad", [](GameObject &object) { object.AddComponent<Example::QuadRenderer>(); }},
                {"Light", "Light", [](GameObject &object) { object.AddComponent<Rendering::Light>(); }},
                {"DirectionalLight", "Directional Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Directional;
                }},
                {"PointLight", "Point Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Point;
                }},
                {"SpotLight", "Spot Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Spot;
                }},
            };
            return presets;
        }

        /// The name a duplicate of `source` gets among `siblings`, as Unity names it: "Cube" and "Cube (3)" give
        /// "Cube (1)", then "Cube (2)", ... the first of those no sibling has
        std::string DuplicateName(const GameObject &source, const std::vector<std::shared_ptr<GameObject>> &siblings)
        {
            std::string base = source.GetName();
            // "Cube (3)" is a duplicate of "Cube", not of itself
            if (base.size() > 4 && base.back() == ')')
            {
                const size_t open = base.rfind(" (");
                if (open != std::string::npos && base.size() - open >= 4)
                {
                    const std::string_view digits = std::string_view(base).substr(open + 2, base.size() - open - 3);
                    if (!digits.empty() && std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; }))
                    {
                        base.erase(open);
                    }
                }
            }
            for (int number = 1;; ++number)
            {
                std::string candidate = std::format("{} ({})", base, number);
                const bool taken = std::ranges::any_of(siblings, [&candidate](const std::shared_ptr<GameObject> &sibling)
                {
                    return sibling && sibling->GetName() == candidate;
                });
                if (!taken)
                {
                    return candidate;
                }
            }
        }

        /// A resolver that knows every object and component of the scene, by UUID
        ReferenceResolver SceneReferences(const Scene &scene)
        {
            ReferenceResolver references;
            scene.TraverseAll([&references](const std::shared_ptr<GameObject> &gameObject)
            {
                references.RegisterGameObject(gameObject->GetUUID(), gameObject.get());
                for (const auto &component : gameObject->GetAllComponents())
                {
                    references.RegisterComponent(component->GetUUID(), component.get());
                }
            });
            return references;
        }

        // ==================== Components (#6, E5) ====================

        /// The component of the object with this UUID string, or nullptr (also for text that isn't a UUID)
        Component *FindComponentOn(const GameObject &entity, const std::string &componentId)
        {
            const auto uuid = Math::UUID::FromString(componentId);
            if (!uuid.has_value())
            {
                return nullptr;
            }
            for (const auto &component : entity.GetAllComponents())
            {
                if (component->GetUUID() == uuid.value())
                {
                    return component.get();
                }
            }
            return nullptr;
        }

        struct ComponentTarget
        {
            std::shared_ptr<GameObject> entity;
            Component *component = nullptr;
        };

        /// The object and the component of the loaded scene a command names, or the message that says why not
        std::expected<ComponentTarget, std::string> FindComponentTarget(const std::string &entityId,
                                                                        const std::string &componentId)
        {
            Scene *scene = SceneManager::GetCurScene();
            if (scene == nullptr)
            {
                return std::unexpected("No scene loaded");
            }
            ComponentTarget target;
            target.entity = FindEntity(*scene, entityId);
            if (target.entity == nullptr)
            {
                return std::unexpected(NotFoundMessage("Entity", entityId));
            }
            target.component = FindComponentOn(*target.entity, componentId);
            if (target.component == nullptr)
            {
                return std::unexpected(NotFoundMessage("Component", componentId));
            }
            return target;
        }

        /// The UUID strings a reference field's value names, in order, nulls left out
        std::vector<std::string> ReferencedIds(const FieldInfo &field, const nlohmann::json &value)
        {
            std::vector<std::string> ids;
            const auto add = [&ids](const nlohmann::json &one)
            {
                if (one.is_string())
                {
                    const std::string text = one.get<std::string>();
                    // Spelled the one way, so two spellings of an id compare equal
                    const auto uuid = Math::UUID::FromString(text);
                    ids.push_back(uuid.has_value() ? uuid->ToString() : text);
                }
            };
            switch (field.kind)
            {
            case FieldKind::AssetRef:
            case FieldKind::GameObjectRef:
            case FieldKind::ComponentRef:
                // Inside a container (a script's fields) a reference is {"$ref": id}
                add(field.container.empty() ? value : value.at("$ref"));
                break;
            case FieldKind::AssetRefList:
            case FieldKind::GameObjectRefList:
            case FieldKind::ComponentRefList:
                for (const nlohmann::json &one : value)
                {
                    add(one);
                }
                break;
            default:
                break;
            }
            return ids;
        }

        /// The references of a value that the scene (or the project's assets) can already be seen not to satisfy:
        /// an object or component that isn't in the scene, a component of another type than the field's, an asset
        /// of another type than the field's. (An asset that resolves to nothing is found by the copy.)
        std::optional<std::string> CheckFieldReferences(const FieldInfo &field, const nlohmann::json &value,
                                                        const ReferenceResolver &sceneReferences)
        {
            for (const std::string &id : ReferencedIds(field, value))
            {
                const auto uuid = Math::UUID::FromString(id);
                if (!uuid.has_value())
                {
                    continue;
                }
                switch (field.kind)
                {
                case FieldKind::GameObjectRef:
                case FieldKind::GameObjectRefList:
                    if (sceneReferences.FindGameObject(uuid.value()) == nullptr)
                    {
                        return std::format("Field '{}': object {} isn't in the scene", field.name, id);
                    }
                    break;
                case FieldKind::ComponentRef:
                case FieldKind::ComponentRefList:
                {
                    const Component *target = sceneReferences.FindComponent(uuid.value());
                    if (target == nullptr)
                    {
                        return std::format("Field '{}': component {} isn't in the scene", field.name, id);
                    }
                    if (field.typeName != "Component" && ComponentRegistry::Instance().IsRegistered(field.typeName) &&
                        target->GetTypeName() != field.typeName)
                    {
                        return std::format("Component {} is a {}; field '{}' expects {}", id, target->GetTypeName(),
                                           field.name, field.typeName);
                    }
                    break;
                }
                case FieldKind::AssetRef:
                case FieldKind::AssetRefList:
                {
                    const IO::AssetMetadata *meta = IO::ResourceLoader::Instance().GetMetadata(uuid.value());
                    if (meta != nullptr && !field.assetType.empty() && meta->resourceType != field.assetType)
                    {
                        return std::format("Asset {} is a {}; field '{}' expects {}", id, meta->resourceType,
                                           field.name, field.assetType);
                    }
                    // Nothing knows the UUID (not a project file, a sub-asset, a runtime asset or a built-in mesh):
                    // refused here, since looking it up as a T would load every model of the project to look for it
                    const bool known = meta != nullptr ||
                                       IO::ResourceLoader::Instance().FindSubAssetLocation(uuid.value()) != nullptr ||
                                       IO::Resources::Instance().GetAsset<Base::Asset>(uuid.value()) != nullptr ||
                                       Rendering::Mesh::FindBuiltin(uuid.value()) != nullptr;
                    if (!known)
                    {
                        return std::format("Field '{}': no {} asset has the UUID {}", field.name,
                                           field.assetType.empty() ? std::string("such") : field.assetType, id);
                    }
                    break;
                }
                default:
                    break;
                }
            }
            return std::nullopt;
        }

        /// SetComponentFields: checks `values` (every key a field of the component, every value of its kind, every
        /// reference something the scene or project has), applies them to a copy made from a second instance of the
        /// type (a failure there, or a reference that came out other than asked for, is the answer), and only then
        /// to the component itself, which can't then fail midway. Returns the values the component saves now, and
        /// in `changedKeys` the top-level keys of `values` whose saved value moved.
        std::expected<nlohmann::json, std::string> SetComponentValues(Scene &scene, Component &component,
                                                                      const nlohmann::json &requested,
                                                                      std::vector<std::string> &changedKeys)
        {
            nlohmann::json values = requested;
            const std::vector<FieldInfo> fields = component.DescribeFields();
            if (values.is_object())
            {
                // What GetComponent returned can be sent back: a key that is no field (the uuid, a LuaComponent's
                // scriptPath, the scriptData of one without a script) is accepted when it is what the component
                // has, and refused otherwise
                const nlohmann::json current = component.Serialize();
                std::vector<std::string> echoes;
                for (const auto &[key, value] : values.items())
                {
                    const bool isField = key == "isActive" || std::ranges::any_of(fields, [&key](const FieldInfo &field)
                    {
                        return field.name == key || field.container == key;
                    });
                    if (!isField && current.contains(key) && value == current.at(key))
                    {
                        echoes.push_back(key);
                    }
                }
                for (const std::string &key : echoes)
                {
                    values.erase(key);
                }
                // The check below validates scriptData against the script the component has now; a request that
                // also changes the script would be checked against the wrong one
                if (values.contains("scriptUUID") && values.contains("scriptData") &&
                    values.at("scriptUUID") != current.value("scriptUUID", nlohmann::json()))
                {
                    return std::unexpected("Set the script (scriptUUID) and its data (scriptData) in separate requests");
                }
            }
            if (const auto problem = ValidateFieldValues(fields, values))
            {
                return std::unexpected(EditorServer::SanitizeForLog(*problem, 300));
            }

            // Every field a key names, with the value it is given (a container's own fields included)
            struct Given
            {
                const FieldInfo *field;
                const nlohmann::json *value;
            };
            std::vector<Given> given;
            for (const auto &[key, value] : values.items())
            {
                if (key == "isActive")
                {
                    continue;
                }
                for (const FieldInfo &field : fields)
                {
                    if (field.container.empty() && field.name == key)
                    {
                        given.push_back({&field, &value});
                    }
                    else if (!field.container.empty() && field.container == key && value.contains(field.name))
                    {
                        given.push_back({&field, &value.at(field.name)});
                    }
                }
            }

            const ReferenceResolver sceneReferences = SceneReferences(scene);
            for (const Given &one : given)
            {
                if (const auto problem = CheckFieldReferences(*one.field, *one.value, sceneReferences))
                {
                    return std::unexpected(EditorServer::SanitizeForLog(*problem, 300));
                }
            }

            // The copy: another instance of the type, on an object in no scene, so it is never attached
            const std::shared_ptr<GameObject> holder = GameObject::Create("EditorScratch");
            const std::unique_ptr<Component> scratch = ComponentRegistry::Instance().Create(component.GetTypeName(), *holder);
            if (scratch == nullptr)
            {
                return std::unexpected(std::format("Component type '{}' isn't registered, so it can't be edited",
                                                   EditorServer::SanitizeForLog(component.GetTypeName())));
            }
            ReferenceResolver scratchReferences;
            scratchReferences.SetFallback(&sceneReferences);
            try
            {
                scratch->SetEditorFields(values, &scratchReferences);
                scratchReferences.ResolveAll();
            }
            catch (const nlohmann::json::exception &error)
            {
                return std::unexpected(EditorServer::SanitizeForLog(std::format("Invalid value: {}", error.what()), 300));
            }
            catch (const std::exception &error)
            {
                return std::unexpected(EditorServer::SanitizeForLog(error.what(), 300));
            }

            // A reference the copy didn't keep (an asset that isn't there or isn't of the field's type, a component
            // that isn't a T) was dropped by its deserialiser; the request is refused rather than half kept
            const nlohmann::json copied = scratch->Serialize();
            for (const Given &one : given)
            {
                if (!one.field->container.empty() || !copied.contains(one.field->name))
                {
                    continue;
                }
                const std::vector<std::string> asked = ReferencedIds(*one.field, *one.value);
                const std::vector<std::string> kept = ReferencedIds(*one.field, copied.at(one.field->name));
                if (asked != kept)
                {
                    const auto lost = std::ranges::find_if(asked, [&kept](const std::string &id)
                    {
                        return std::ranges::find(kept, id) == kept.end();
                    });
                    const std::string what = (one.field->kind == FieldKind::AssetRef || one.field->kind == FieldKind::AssetRefList)
                                                 ? std::format("no {} asset has the UUID {}", one.field->assetType.empty() ? std::string("such") : one.field->assetType,
                                                               lost != asked.end() ? *lost : std::string{})
                                                 : std::format("{} can't be used", lost != asked.end() ? *lost : std::string{});
                    return std::unexpected(EditorServer::SanitizeForLog(std::format("Field '{}': {}", one.field->name, what), 300));
                }
            }

            // The component itself
            const nlohmann::json before = component.Serialize();
            ReferenceResolver references;
            references.SetFallback(&sceneReferences);
            try
            {
                component.SetEditorFields(values, &references);
                references.ResolveAll();
                if (const auto active = values.find("isActive"); active != values.end())
                {
                    component.SetActive(active->get<bool>());
                }
            }
            catch (const std::exception &error)
            {
                // The copy took it, so this isn't expected: put back what was saved rather than leave half of it
                ReferenceResolver restore;
                restore.SetFallback(&sceneReferences);
                component.Deserialize(before, &restore);
                restore.ResolveAll();
                return std::unexpected(EditorServer::SanitizeForLog(error.what(), 300));
            }

            nlohmann::json after = component.Serialize();
            for (const auto &[key, value] : values.items())
            {
                if (!before.contains(key) || !after.contains(key) || before.at(key) != after.at(key))
                {
                    changedKeys.push_back(key);
                }
            }
            return after;
        }

        /// SetEntityProperties' properties, checked
        struct EntityProperties
        {
            std::optional<std::string> name;
            std::optional<bool> active;
            std::optional<std::string> tag;
            std::optional<int> layer;
        };

        std::expected<EntityProperties, std::string> ParseEntityProperties(const nlohmann::json &properties)
        {
            if (!properties.is_object())
            {
                return std::unexpected("properties must be a JSON object with any of name, active, tag and layer");
            }
            EntityProperties parsed;
            for (const auto &[key, value] : properties.items())
            {
                if (key == "name" || key == "tag")
                {
                    if (!value.is_string())
                    {
                        return std::unexpected(std::format("{} must be a string", key));
                    }
                    (key == "name" ? parsed.name : parsed.tag) = value.get<std::string>();
                }
                else if (key == "active")
                {
                    if (!value.is_boolean())
                    {
                        return std::unexpected("active must be true or false");
                    }
                    parsed.active = value.get<bool>();
                }
                else if (key == "layer")
                {
                    // Not clamped: a client that means layer 40 has a bug, and finds out
                    if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() >= Layers::Count)
                    {
                        return std::unexpected(std::format("layer must be an integer from 0 to {}", Layers::Count - 1));
                    }
                    parsed.layer = static_cast<int>(value.get<int64_t>());
                }
                else
                {
                    return std::unexpected(std::format("Unknown property '{}' (the properties are name, active, tag and "
                                                       "layer)", EditorServer::SanitizeForLog(key)));
                }
            }
            return parsed;
        }

        // ==================== Undo and redo (#6, E6) ====================
        // The halves of an edit below address what they change by UUID, looked up when they run (never by pointer),
        // and set a state captured when the edit was made (not "the opposite of what was asked"), so an undo is exact
        // whatever the request did (a clamped field, a rotation that was normalised, a transform given to an object
        // that had none).

        /// The loaded scene, or the error an edit answers when it runs without one
        std::expected<Scene *, std::string> LoadedScene()
        {
            Scene *scene = SceneManager::GetCurScene();
            if (scene == nullptr)
            {
                return std::unexpected(std::string("No scene loaded"));
            }
            return scene;
        }

        std::expected<std::shared_ptr<GameObject>, std::string> LoadedEntity(const std::string &entityId)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            std::shared_ptr<GameObject> entity = FindEntity(**scene, entityId);
            if (entity == nullptr)
            {
                return std::unexpected(NotFoundMessage("Entity", entityId));
            }
            return entity;
        }

        EditEffect EffectFor(std::vector<std::string> ids)
        {
            EditEffect effect;
            effect.entityIds = std::move(ids);
            return effect;
        }

        /// JSON kept as text in a step: a parsed nlohmann::json costs several times its text, which the history's byte
        /// limit would not see, so a step holds the text (as a snapshot does) and parses it when it runs
        std::shared_ptr<const std::string> DumpJson(const nlohmann::json &value)
        {
            return std::make_shared<const std::string>(
                value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        }

        std::expected<nlohmann::json, std::string> ParseStored(const std::string &text)
        {
            nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
            if (value.is_discarded())
            {
                return std::unexpected(std::string("What the edit saved can't be read back"));
            }
            return value;
        }

        /// "Move Cube": a step's label, with the object's name made safe and short
        std::string Labelled(const std::string_view verb, const std::string_view name)
        {
            return std::format("{} {}", verb, EditorServer::SanitizeForLog(name, 60));
        }

        /// An object's local transform, or that it has none
        struct LocalTransformState
        {
            bool present = false;
            Math::Vector3 position{0.0f, 0.0f, 0.0f};
            Math::Quaternion rotation;
            Math::Vector3 scale{1.0f, 1.0f, 1.0f};

            /// Exactly the same, number for number: Vector3's and Quaternion's operator== allow a small difference, and a
            /// change that small is still a change (it moves the scene revision and the frame)
            [[nodiscard]] bool SameAs(const LocalTransformState &other) const
            {
                return present == other.present && position.x == other.position.x && position.y == other.position.y &&
                       position.z == other.position.z && rotation.GetX() == other.rotation.GetX() &&
                       rotation.GetY() == other.rotation.GetY() && rotation.GetZ() == other.rotation.GetZ() &&
                       rotation.GetW() == other.rotation.GetW() && scale.x == other.scale.x &&
                       scale.y == other.scale.y && scale.z == other.scale.z;
            }
        };

        /// Text for a transform: equal text for equal numbers, which is all a group needs to drop an op. It is
        /// conservative: +0 and -0 print differently, so that op is kept; two NaNs print alike, which only drops an
        /// op that undo and redo would apply as the same text-equal state anyway.
        std::shared_ptr<const std::string> Fingerprint(const LocalTransformState &state)
        {
            return std::make_shared<const std::string>(std::format(
                "{} {:a} {:a} {:a} {:a} {:a} {:a} {:a} {:a} {:a} {:a}", state.present, state.position.x, state.position.y,
                state.position.z, state.rotation.GetX(), state.rotation.GetY(), state.rotation.GetZ(),
                state.rotation.GetW(), state.scale.x, state.scale.y, state.scale.z));
        }

        LocalTransformState LocalTransformOf(const GameObject &gameObject)
        {
            LocalTransformState state;
            if (const Positionable *positionable = gameObject.GetPositionable())
            {
                state.present = true;
                state.position = positionable->GetLocalPosition();
                state.rotation = positionable->GetLocalRotation();
                state.scale = positionable->GetLocalScale();
            }
            return state;
        }

        /// Sets the local transform. A state without one (the object had none) puts the identity on an object that
        /// has one by now: a transform can't be taken away, and the identity is where an object without one is. False
        /// then: the state isn't exactly the one that was captured (the scene saves a "positionable" it didn't).
        bool ApplyLocalTransform(GameObject &gameObject, const LocalTransformState &state)
        {
            if (!state.present && gameObject.GetPositionable() == nullptr)
            {
                return true;
            }
            gameObject.CreatePositionable();
            Positionable *positionable = gameObject.GetPositionable();
            positionable->SetLocalPositionAndRotation(state.position, state.rotation);
            positionable->SetLocalScale(state.scale);
            return state.present;
        }

        /// SetLocalTransform and SetEntityTransform: coalesces with the next transform edit of the same object, and
        /// merges past other objects' in a group (dragging a selection moves several every frame)
        EditOp TransformOp(const std::string &entityId, const LocalTransformState &before,
                           const LocalTransformState &after)
        {
            const auto apply = [entityId](const LocalTransformState &state) -> EditOutcome
            {
                const auto entity = LoadedEntity(entityId);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const bool exact = ApplyLocalTransform(**entity, state);
                // The children's world matrices moved with it
                EditEffect effect = EffectFor(SubtreeIds(**entity));
                effect.inexact = !exact;
                return effect;
            };
            EditOp op;
            op.undo = [apply, before]() -> EditOutcome { return apply(before); };
            op.redo = [apply, after]() -> EditOutcome { return apply(after); };
            op.coalesceKey = "transform:" + entityId;
            op.commutes = true;
            op.undoState = Fingerprint(before);
            op.redoState = Fingerprint(after);
            op.undoBytes = op.undoState->size();
            op.redoBytes = op.redoState->size();
            return op;
        }

        /// An object's name, tag, layer and own active flag
        struct PropertiesState
        {
            std::string name;
            std::string tag;
            int layer = 0;
            bool active = true;
        };

        PropertiesState PropertiesOf(const GameObject &gameObject)
        {
            return {gameObject.GetName(), gameObject.GetTag(), gameObject.GetLayer(), gameObject.IsActive()};
        }

        EditEffect ApplyProperties(GameObject &gameObject, const PropertiesState &state)
        {
            const bool activeChanged = gameObject.IsActive() != state.active;
            if (gameObject.GetName() != state.name)
            {
                gameObject.SetName(state.name);
            }
            if (gameObject.GetTag() != state.tag)
            {
                gameObject.SetTag(state.tag);
            }
            if (gameObject.GetLayer() != state.layer)
            {
                gameObject.SetLayer(state.layer);
            }
            if (activeChanged)
            {
                gameObject.SetActive(state.active);
            }
            // The active state reaches the whole subtree (activeInHierarchy), as in SetEntityProperties
            return EffectFor(activeChanged ? SubtreeIds(gameObject)
                                           : std::vector<std::string>{gameObject.GetUUID().ToString()});
        }

        /// Text for a state: equal states give equal text (the name and tag are sized, so none runs into the next)
        std::shared_ptr<const std::string> Fingerprint(const PropertiesState &state)
        {
            return std::make_shared<const std::string>(std::format("{}:{}:{}:{}:{}:{}", state.name.size(), state.name,
                                                                    state.tag.size(), state.tag, state.layer, state.active));
        }

        /// SetEntityProperties: `changed` names the properties it changed ("name", ...), so typing a name coalesces
        EditOp PropertiesOp(const std::string &entityId, const PropertiesState &before, const PropertiesState &after,
                            const std::string &changed)
        {
            const auto apply = [entityId](const PropertiesState &state) -> EditOutcome
            {
                const auto entity = LoadedEntity(entityId);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                return ApplyProperties(**entity, state);
            };
            EditOp op;
            op.undo = [apply, before]() -> EditOutcome { return apply(before); };
            op.redo = [apply, after]() -> EditOutcome { return apply(after); };
            op.undoBytes = before.name.size() + before.tag.size();
            op.redoBytes = after.name.size() + after.tag.size();
            op.coalesceKey = "properties:" + entityId + ":" + changed;
            op.undoState = Fingerprint(before);
            op.redoState = Fingerprint(after);
            return op;
        }

        /// Where an object is in the hierarchy: its parent (empty for a root), its place among the siblings and its
        /// local transform (reparenting can keep the world transform, which changes the local one)
        struct PlacementState
        {
            std::string parentId;
            size_t index = 0;
            LocalTransformState transform;
        };

        PlacementState PlacementOf(const GameObject &gameObject)
        {
            return {ParentIdOf(gameObject), gameObject.GetSiblingIndex(), LocalTransformOf(gameObject)};
        }

        EditOutcome ApplyPlacement(const std::string &entityId, const PlacementState &state)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            const auto entity = LoadedEntity(entityId);
            if (!entity)
            {
                return std::unexpected(entity.error());
            }
            std::shared_ptr<GameObject> parent;
            if (!state.parentId.empty())
            {
                parent = FindEntity(**scene, state.parentId);
                if (parent == nullptr)
                {
                    return std::unexpected(NotFoundMessage("Parent", state.parentId));
                }
                if (parent == *entity || parent->IsChildOf(*entity))
                {
                    return std::unexpected(std::string("The object can't be a child of itself or of its own descendant"));
                }
            }
            if ((*entity)->GetParent() != parent)
            {
                // The local transform is put back below, so the world transform needn't be kept
                (*entity)->SetParent(parent, false);
                if ((*entity)->GetParent() != parent)
                {
                    return std::unexpected(std::string("The object couldn't be moved back"));
                }
            }
            const bool exact = ApplyLocalTransform(**entity, state.transform);
            (*entity)->SetSiblingIndex(state.index);
            EditEffect effect = EffectFor(SubtreeIds(**entity));
            effect.inexact = !exact;
            return effect;
        }

        EditOp PlacementOp(const std::string &entityId, const PlacementState &before, const PlacementState &after)
        {
            EditOp op;
            op.undo = [entityId, before]() -> EditOutcome { return ApplyPlacement(entityId, before); };
            op.redo = [entityId, after]() -> EditOutcome { return ApplyPlacement(entityId, after); };
            return op;
        }

        /// Drops every reference to an object and its subtree (and to their components) that other components of the
        /// scene hold, before they are destroyed. A reference is a raw pointer that nothing tracks, so the scene would be
        /// saved (the autosave does, after every edit) and shown with pointers to freed objects. Undoing a destroy puts
        /// the references back by loading the scene as it was, which resolves them again by UUID.
        /// The UUIDs of the objects whose components hold a reference `isObject` or `isComponent` says yes to (an empty
        /// function says no to everything). With `clear` the references are dropped, as ForgetGameObjectsIf and
        /// ForgetComponentsIf do; without, they are left as they are, which finds who a change that drops them itself
        /// (RemoveComponent) will touch. `skip` is a component that isn't asked.
        std::vector<std::string> ReferenceHolders(Scene &scene, const std::function<bool(const GameObject *)> &isObject,
                                                  const std::function<bool(const Component *)> &isComponent,
                                                  const bool clear, const Component *skip = nullptr)
        {
            std::vector<std::string> holders;
            scene.TraverseAll([&](const std::shared_ptr<GameObject> &holder)
            {
                bool held = false;
                const std::function<bool(const GameObject *)> objectProbe = [&](const GameObject *candidate)
                {
                    const bool matches = isObject && isObject(candidate);
                    held = held || matches;
                    return clear && matches;
                };
                const std::function<bool(const Component *)> componentProbe = [&](const Component *candidate)
                {
                    const bool matches = isComponent && isComponent(candidate);
                    held = held || matches;
                    return clear && matches;
                };
                for (const auto &other : holder->GetAllComponents())
                {
                    if (other.get() == skip)
                    {
                        continue;
                    }
                    // Only the kind asked for: a pass over the members costs, and the other kind matches nothing
                    if (isObject)
                    {
                        other->ForgetGameObjectsIf(objectProbe);
                    }
                    if (isComponent)
                    {
                        other->ForgetComponentsIf(componentProbe);
                    }
                }
                if (held)
                {
                    holders.push_back(holder->GetUUID().ToString());
                }
            });
            return holders;
        }

        /// The objects other than the one `component` is on whose references to it a removal will clear (none unless
        /// the scene is opened for editing, the only one that clears them). Asked through ForgetComponentsIf, which is
        /// what a SerializableComponent's ForgetComponent does; a component that overrides ForgetComponent alone is
        /// cleared by the removal but not listed (see the docs).
        std::vector<std::string> HoldersOfComponent(Scene &scene, const Component &component)
        {
            if (!scene.IsEditMode())
            {
                return {};
            }
            return ReferenceHolders(scene, {}, [&component](const Component *candidate) { return candidate == &component; },
                                    false, &component);
        }

        /// Adds the ids in `more` that `ids` doesn't have
        void AppendNewIds(std::vector<std::string> &ids, std::vector<std::string> more)
        {
            std::unordered_set<std::string> known(ids.begin(), ids.end());
            for (std::string &id : more)
            {
                if (known.insert(id).second)
                {
                    ids.push_back(std::move(id));
                }
            }
        }

        /// Returns the objects that held a reference which was dropped, for the change event (the destroyed ones too,
        /// if they held one: the caller lists those already)
        std::vector<std::string> ForgetDestroyed(Scene &scene, const GameObject &root)
        {
            std::unordered_set<const GameObject *> objects{&root};
            std::unordered_set<const Component *> components;
            for (const auto &descendant : root.GetChildrenRecursive())
            {
                objects.insert(descendant.get());
            }
            for (const GameObject *object : objects)
            {
                for (const auto &component : object->GetAllComponents())
                {
                    components.insert(component.get());
                }
            }

            // One pass over the scene, and one over each holder's members, whatever the size of the subtree
            const std::function<bool(const GameObject *)> removedObject = [&objects](const GameObject *candidate)
            {
                return objects.contains(candidate);
            };
            const std::function<bool(const Component *)> removedComponent = [&components](const Component *candidate)
            {
                return components.contains(candidate);
            };
            return ReferenceHolders(scene, removedObject, removedComponent, true);
        }

        /// Destroys an object with everything under it, as DestroyEntity does
        EditOutcome DestroySubtree(const std::string &entityId)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            const auto entity = LoadedEntity(entityId);
            if (!entity)
            {
                return std::unexpected(entity.error());
            }
            std::vector<std::string> ids = SubtreeIds(**entity);
            if (!(*scene)->DestroyGameObject(*entity))
            {
                return std::unexpected(std::string("The object couldn't be destroyed"));
            }
            if ((*scene)->IsEditMode())
            {
                // The objects that held a reference to something destroyed lost it: their inspectors are stale too
                AppendNewIds(ids, ForgetDestroyed(**scene, **entity));
            }
            (*scene)->ProcessDestroyed();
            return EffectFor(std::move(ids));
        }

        /// Builds an object tree again from its saved form, with the UUIDs it had, and puts it back in its place. A
        /// reference to something outside it resolves through the scene, as for a duplicate.
        EditOutcome RestoreSubtree(const nlohmann::json &subtree, const std::string &parentId, const size_t index)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            std::shared_ptr<GameObject> parent;
            if (!parentId.empty())
            {
                parent = FindEntity(**scene, parentId);
                if (parent == nullptr)
                {
                    return std::unexpected(NotFoundMessage("Parent", parentId));
                }
            }

            const ReferenceResolver sceneReferences = SceneReferences(**scene);
            ReferenceResolver resolver;
            resolver.SetFallback(&sceneReferences);
            std::shared_ptr<GameObject> restored;
            try
            {
                restored = GameObject::Deserialize(subtree, &resolver);
                resolver.ResolveAll();
            }
            catch (const std::exception &error)
            {
                return std::unexpected(EditorServer::SanitizeForLog(error.what(), 300));
            }
            if (restored == nullptr)
            {
                return std::unexpected(std::string("The object couldn't be built again"));
            }

            if (parent != nullptr)
            {
                parent->AddChild(restored, false);
            }
            else
            {
                (*scene)->AddRootGameObject(restored);
            }
            restored->SetSiblingIndex(index);
            return EffectFor(SubtreeIds(*restored));
        }

        /// CreateEntity, CreateEntityEx and DuplicateEntity: undone by destroying the new tree, redone by building it
        /// again from the saved form it had when it was made. `created` is in its place already.
        EditOp CreatedOp(const GameObject &created)
        {
            const std::string entityId = created.GetUUID().ToString();
            const std::string parentId = ParentIdOf(created);
            const size_t index = created.GetSiblingIndex();
            const auto subtree = DumpJson(created.Serialize());

            EditOp op;
            op.undo = [entityId]() -> EditOutcome { return DestroySubtree(entityId); };
            op.redo = [subtree, parentId, index]() -> EditOutcome
            {
                const auto saved = ParseStored(*subtree);
                if (!saved)
                {
                    return std::unexpected(saved.error());
                }
                return RestoreSubtree(*saved, parentId, index);
            };
            op.redoBytes = subtree->size();
            return op;
        }

        /// Removes a component by its UUID, as RemoveComponent does
        EditOutcome RemoveComponentById(const std::string &entityId, const std::string &componentId)
        {
            const auto target = FindComponentTarget(entityId, componentId);
            if (!target)
            {
                return std::unexpected(target.error());
            }
            // Found before the removal clears what they hold
            std::vector<std::string> ids{entityId};
            if (const auto scene = LoadedScene(); scene)
            {
                AppendNewIds(ids, HoldersOfComponent(**scene, *target->component));
            }
            if (!target->entity->RemoveComponent(target->component))
            {
                return std::unexpected(std::string("The component couldn't be removed"));
            }
            return EffectFor(std::move(ids));
        }

        /// Adds a component of a type, built from the saved form it had (its UUID included), to an object
        EditOutcome AddComponentFromValues(const std::string &entityId, const std::string &typeName,
                                           const nlohmann::json &values)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            const auto entity = LoadedEntity(entityId);
            if (!entity)
            {
                return std::unexpected(entity.error());
            }
            std::unique_ptr<Component> component = ComponentRegistry::Instance().Create(typeName, **entity);
            if (component == nullptr)
            {
                return std::unexpected(std::format("Component type '{}' isn't registered",
                                                   EditorServer::SanitizeForLog(typeName)));
            }

            const ReferenceResolver sceneReferences = SceneReferences(**scene);
            ReferenceResolver resolver;
            resolver.SetFallback(&sceneReferences);
            try
            {
                if (values.contains("uuid") && values.at("uuid").is_string())
                {
                    // A reference the component holds to itself resolves to it
                    if (const auto uuid = Math::UUID::FromString(values.at("uuid").get<std::string>()); uuid.has_value())
                    {
                        resolver.RegisterComponent(uuid.value(), component.get());
                    }
                }
                component->Deserialize(values, &resolver);
                resolver.ResolveAll();
            }
            catch (const std::exception &error)
            {
                return std::unexpected(EditorServer::SanitizeForLog(error.what(), 300));
            }
            if ((*entity)->AddComponent(std::move(component)) == nullptr)
            {
                return std::unexpected(std::string("The component couldn't be added"));
            }
            return EffectFor({entityId});
        }

        /// AddComponent: undone by removing the component, redone by adding one again from the values it had
        EditOp AddedComponentOp(const std::string &entityId, const std::string &componentId, const std::string &typeName,
                                const nlohmann::json &values)
        {
            const auto saved = DumpJson(values);
            EditOp op;
            op.undo = [entityId, componentId]() -> EditOutcome { return RemoveComponentById(entityId, componentId); };
            op.redo = [entityId, typeName, saved]() -> EditOutcome
            {
                const auto parsed = ParseStored(*saved);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }
                return AddComponentFromValues(entityId, typeName, *parsed);
            };
            op.redoBytes = saved->size();
            return op;
        }

        /// Sets a component to a saved form (what Serialize returned), references resolved through the scene, as the
        /// restore after a failed SetComponentFields does; `changed` are the keys OnEditorFieldsChanged is told of
        EditOutcome ApplyComponentState(const std::string &entityId, const std::string &componentId,
                                        const nlohmann::json &state, const std::vector<std::string> &changed)
        {
            const auto scene = LoadedScene();
            if (!scene)
            {
                return std::unexpected(scene.error());
            }
            const auto target = FindComponentTarget(entityId, componentId);
            if (!target)
            {
                return std::unexpected(target.error());
            }
            const ReferenceResolver sceneReferences = SceneReferences(**scene);
            ReferenceResolver resolver;
            resolver.SetFallback(&sceneReferences);
            // Deserialize sets the active flag without telling the component (OnActiveFlagChanged), which SetActive does
            // as SetComponentFields did: so the flag is taken out of what is deserialised and set after it
            nlohmann::json withoutActive = state;
            std::optional<bool> active;
            if (withoutActive.is_object())
            {
                if (const auto flag = withoutActive.find("isActive"); flag != withoutActive.end() && flag->is_boolean())
                {
                    active = flag->get<bool>();
                    withoutActive.erase(flag);
                }
            }
            try
            {
                target->component->Deserialize(withoutActive, &resolver);
                resolver.ResolveAll();
                if (active.has_value() && target->component->IsActive() != *active)
                {
                    target->component->SetActive(*active);
                }
                target->component->OnEditorFieldsChanged(changed);
            }
            catch (const std::exception &error)
            {
                return std::unexpected(EditorServer::SanitizeForLog(error.what(), 300));
            }
            return EffectFor({entityId});
        }

        /// SetComponentFields: before and after are the component's saved form; typing in one field coalesces
        EditOp ComponentFieldsOp(const std::string &entityId, const std::string &componentId,
                                 const nlohmann::json &before, const nlohmann::json &after,
                                 const std::vector<std::string> &changed, const std::string &requestedKeys)
        {
            const auto beforeState = DumpJson(before);
            const auto afterState = DumpJson(after);
            // `changed` is what the request named, the same for every edit that merges into this step (the key has it),
            // so both halves tell the component the same keys
            const auto restore = [entityId, componentId, changed](const std::string &text) -> EditOutcome
            {
                const auto state = ParseStored(text);
                if (!state)
                {
                    return std::unexpected(state.error());
                }
                return ApplyComponentState(entityId, componentId, *state, changed);
            };
            EditOp op;
            op.undo = [restore, beforeState]() -> EditOutcome { return restore(*beforeState); };
            op.redo = [restore, afterState]() -> EditOutcome { return restore(*afterState); };
            op.undoBytes = beforeState->size();
            op.redoBytes = afterState->size();
            op.coalesceKey = "fields:" + entityId + ":" + componentId + ":" + requestedKeys;
            // The saved forms are the states. Equal text drops the op; that is safe even where the dump isn't exact
            // (invalid UTF-8 is replaced): undo and redo both apply that same text, so they would set the same state
            op.undoState = beforeState;
            op.redoState = afterState;
            return op;
        }
    }

    EditorServer::EditorServer()
    {
        // Every line logged while the server exists becomes a log event. The subscriber runs on the thread that logged,
        // holding the Logger's lock, so it only copies the line into the ring (under the ring's own mutex) and never logs
        // or waits on another thread (see Logger.hpp)
        _logSubscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        {
            try
            {
                _events.PushLog(level, message, EventRing::NowUnixMilliseconds());
            }
            catch (...)
            {
                // (bad_alloc) Logging must never fail because the editor couldn't keep a copy of the line
            }
        };
    }

    EditorServer::~EditorServer()
    {
        Stop();
        // The keyboard this server installed goes with it
        if (Input::KeySource::Get() == &_keys)
        {
            Input::KeySource::Set(nullptr);
        }
        // Once this returns no subscriber call is running (dispatch holds the Logger's lock), so _events can go
        Logger::logEvent -= _logSubscription;
    }

    bool EditorServer::Start(int port, const std::string &bindAddress)
    {
        if (_running) return true;
        // A client Shutdown stops serving without Stop(): join that thread before starting another
        if (_serverThread.joinable()) Stop();

#ifdef _WIN32
        if (!_socketsInitialized)
        {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            {
                Logger::Error("WSAStartup failed");
                return false;
            }
        }
#endif
        _socketsInitialized = true;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1)
        {
            Logger::Error("Invalid bind address (expected an IPv4 address): " + bindAddress);
            return false;
        }

        _listenSocket = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (_listenSocket == INVALID_SOCKET)
        {
            Logger::Error("Failed to create socket");
            _listenSocket = -1;
            return false;
        }

#ifndef _WIN32
        // Lets a restarted host rebind while the last connection is in TIME_WAIT. Not on Windows, where
        // SO_REUSEADDR allows hijacking the port, and the default already rebinds over TIME_WAIT.
        int opt = 1;
        setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

        if (bind(_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            Logger::Error("Failed to bind to " + bindAddress + ":" + std::to_string(port));
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return false;
        }

        if (listen(_listenSocket, 1) < 0)
        {
            Logger::Error("Failed to listen");
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return false;
        }

        // The actual port, in case port 0 asked the OS to pick one
        sockaddr_in boundAddr{};
        socklen_t boundAddrSize = sizeof(boundAddr);
        _port = getsockname(_listenSocket, reinterpret_cast<sockaddr*>(&boundAddr), &boundAddrSize) == 0
                    ? ntohs(boundAddr.sin_port)
                    : port;

        _commands.Reopen();
        _running = true;
        _serverThread = std::thread(&EditorServer::ServerLoop, this, _listenSocket);
        Logger::Info("Editor server listening on " + bindAddress + ":" + std::to_string(_port));
        return true;
    }

    void EditorServer::Stop()
    {
        // The network thread never blocks on a socket for long (see WaitUntilReady), so clearing _running
        // stops it within one poll interval. Closing the queue wakes it if it is waiting for a command's
        // result. Sockets are only closed once it has exited: closing or shutting down a socket another
        // thread is blocked on doesn't reliably wake it (shutdown doesn't on Windows, close doesn't on Linux).
        _running = false;
        _commands.Close();

        if (_serverThread.joinable())
            _serverThread.join();

        if (_listenSocket != -1)
        {
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
        }
        _port = 0;

#ifdef _WIN32
        if (_socketsInitialized)
        {
            WSACleanup();
        }
#endif
        _socketsInitialized = false;
    }

    size_t EditorServer::ProcessCommands(std::chrono::milliseconds maxWait)
    {
        const size_t processed = _commands.Drain(maxWait);
        // The autosave an edit left waiting for its interval (a no-op when none is due)
        FlushAutosave();
        // Asset files changed outside the editor (a no-op unless a client is connected and the interval has passed)
        (void)PollAssetsIfDue();
        return processed;
    }

    void EditorServer::UpdateAudio()
    {
        if (_playMode)
        {
            return; // Application::Tick paces the audio with the game
        }
        auto &audio = Audio::AudioSystem::Instance();
        const auto now = std::chrono::steady_clock::now();
        if (_lastAudioUpdate && audio.IsLoopback())
        {
            // AdvanceStream caps one step (a stalled main thread doesn't mix minutes at once)
            audio.AdvanceStream(std::chrono::duration<double>(now - *_lastAudioUpdate).count());
            audio.Update();
        }
        _lastAudioUpdate = now;
    }

    void EditorServer::PostLog(std::string message, bool isWarning)
    {
        // Fire and forget: the future is dropped, and the line is lost if the queue is already closed
        (void)_commands.Enqueue([message = std::move(message), isWarning]
        {
            if (isWarning)
                Logger::Warn(message);
            else
                Logger::Info(message);
            return CommandQueue::Response{};
        });
    }

    bool EditorServer::SetAccessToken(std::string token)
    {
        if (_running)
        {
            Logger::Warn("The editor server's access token can't change while it is running");
            return false;
        }
        _accessToken = std::move(token);
        return true;
    }

    bool EditorServer::SetHelloTimeout(std::chrono::milliseconds timeout)
    {
        if (_running || timeout <= std::chrono::milliseconds::zero())
        {
            Logger::Warn("The editor server's Hello timeout can't change while it is running, or be zero or negative");
            return false;
        }
        _helloTimeout = timeout;
        return true;
    }

    bool EditorServer::SetStopOnDisconnect(const bool stop)
    {
        if (_running)
        {
            Logger::Warn("Whether the editor server stops on disconnect can't change while it is running");
            return false;
        }
        _stopOnDisconnect = stop;
        return true;
    }

    std::string EditorServer::SanitizeForLog(std::string_view text, size_t maxBytes)
    {
        bool truncated = false;
        if (text.size() > maxBytes)
        {
            // Back up over UTF-8 continuation bytes (10xxxxxx), so the cut never splits a character
            size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                --cut;
            text = text.substr(0, cut);
            truncated = true;
        }

        std::string result;
        result.reserve(text.size() + 3);
        for (const char c : text)
        {
            const auto byte = static_cast<unsigned char>(c);
            result += byte < 0x20 || byte == 0x7F ? '?' : c;
        }
        if (truncated)
            result += "...";
        return result;
    }

    std::string_view EditorServer::EngineVersion()
    {
        // The engine's own (CMake's project version), the one project files record
        return N2Engine::EngineVersion();
    }

    std::vector<std::string> EditorServer::Capabilities()
    {
        return {};
    }

    bool EditorServer::TokensMatch(std::string_view accessToken, std::string_view token)
    {
        // Every byte of the access token is compared whatever the token holds; only the length is learnt early
        unsigned int difference = accessToken.size() == token.size() ? 0u : 1u;
        for (size_t i = 0; i < accessToken.size(); ++i)
        {
            const unsigned char given = i < token.size() ? static_cast<unsigned char>(token[i]) : 0;
            difference |= static_cast<unsigned char>(accessToken[i]) ^ given;
        }
        return difference == 0;
    }

    bool EditorServer::IsAllowedBeforeHello(uint8_t commandType)
    {
        return static_cast<CommandType>(commandType) == CommandType::Hello;
    }

    void EditorServer::ServerLoop(int listenSocket)
    {
        while (_running)
        {
            PostLog("Waiting for editor connection...");

            int clientSocket = NoSocket;
            if (WaitUntilReady(listenSocket, false))
            {
                clientSocket = static_cast<int>(accept(listenSocket, nullptr, nullptr));
            }
            if (clientSocket == NoSocket)
            {
                if (_running)
                {
                    PostLog("Accept failed: " + std::to_string(SOCKET_ERROR_CODE), true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // don't spin on a broken listener
                }
                continue;
            }

            PostLog("Editor connected");
            _clientConnected = true;
            bool sessionOpened = false;
            try
            {
                HandleClient(clientSocket, sessionOpened);
            }
            catch (...)
            {
                // e.g. bad_alloc; nothing may escape this thread (that would terminate the host)
                PostLog("Dropping the editor connection after an unexpected error", true);
            }

            CLOSE_SOCKET(clientSocket);
            _clientConnected = false;
            PostLog("Editor disconnected");
            // A group it left open becomes a step (queued: the history is main-thread state)
            (void)_commands.Enqueue([this]
            {
                CloseEditGroups();
                // A play host's keys the client held go up with it (a key held when it crashed would stay down)
                _keys.ReleaseAll();
                // A client that went away may have crashed: the last edits are written, however recent the autosave
                FlushAutosave(true);
                return CommandQueue::Response{};
            });

            // A Shutdown (or Stop) has already cleared _running; this is the client going away on its own
            if (_stopOnDisconnect && sessionOpened && _running)
            {
                PostLog("Stopping the editor server: the client's session ended, and it stops on disconnect");
                _running = false;
            }
        }
    }

    void EditorServer::HandleClient(int clientSocket, bool &sessionOpened)
    {
        // This connection's session: whether its last Hello succeeded. A new connection starts without one, so a
        // client that reconnects must send Hello again.
        bool helloAccepted = false;
        // With an access token, a connection that hasn't succeeded a Hello must not hold the (one-at-a-time) server:
        // it has until this deadline to do so, and is closed after a refused command or a failed Hello
        const bool gated = !_accessToken.empty();
        // Without a token every connection is a session (for SetStopOnDisconnect); with one, from its first good Hello
        sessionOpened = !gated;
        const auto helloDeadline = std::chrono::steady_clock::now() + _helloTimeout;
        const auto deadline = [&]() -> std::optional<std::chrono::steady_clock::time_point>
        {
            if (gated && !helloAccepted)
                return helloDeadline;
            return std::nullopt;
        };
        const auto closeUnauthorized = [this](const std::string &reason)
        {
            PostLog("Closing an editor connection that hasn't sent a successful Hello: " + reason, true);
        };
        const auto logIfHelloTimedOut = [&]
        {
            if (deadline().has_value() && _running && std::chrono::steady_clock::now() >= helloDeadline)
                closeUnauthorized(std::format("no Hello within {} ms", _helloTimeout.count()));
        };

        while (_running)
        {
            // Read command header: [type: 1 byte][length: 4 bytes]
            uint8_t cmdType;
            uint32_t payloadLength;
            if (!Receive(clientSocket, &cmdType, 1, deadline()) ||
                !Receive(clientSocket, &payloadLength, sizeof(payloadLength), deadline()))
            {
                logIfHelloTimedOut();
                break;
            }

            // The length is client-supplied, so check it before allocating. The oversized payload is
            // still in the stream, so the connection can't be resynchronized and is closed. Before a Hello on a
            // gated connection, a payload bigger than any Hello needs is refused the same way.
            const bool beforeHello = deadline().has_value();
            const uint32_t limit = beforeHello ? MaxPayloadBytesBeforeHello : MaxPayloadBytes;
            if (payloadLength > limit)
            {
                const std::string message = std::format("Payload of {} bytes exceeds the {} byte limit{}",
                                                        payloadLength, limit, beforeHello ? " before Hello" : "");
                BufferWriter response;
                WriteError(response, message);
                Send(clientSocket, response.Data().data(), response.Size());
                PostLog(message, true);
                break;
            }

            // Read payload
            std::vector<uint8_t> payload(payloadLength);
            if (payloadLength > 0 && !Receive(clientSocket, payload.data(), payloadLength, deadline()))
            {
                logIfHelloTimedOut();
                break;
            }

            // With an access token, nothing but Hello runs (or is even queued) until a Hello has succeeded, and
            // anything else ends the connection once its Error is sent
            if (gated && !helloAccepted && !IsAllowedBeforeHello(cmdType))
            {
                BufferWriter response;
                WriteError(response, HelloRequiredError);
                Send(clientSocket, response.Data().data(), response.Size());
                closeUnauthorized(std::format("it sent command 0x{:X}", cmdType));
                break;
            }

            if (static_cast<CommandType>(cmdType) == CommandType::Shutdown)
            {
                BufferWriter response;
                WriteOk(response);
                Send(clientSocket, response.Data().data(), response.Size());
                // Stop serving, so the host's main loop sees !IsRunning() and exits cleanly
                PostLog("Shutdown requested by editor");
                // The client chose to shut the host down: what it didn't save is what it chose to drop
                (void)_commands.Enqueue([this]
                {
                    PrepareForShutdown();
                    return CommandQueue::Response{};
                });
                _running = false;
                break;
            }

            // Everything else touches engine state, so it runs on the main thread (see ProcessCommands)
            std::future<CommandQueue::Response> pending = _commands.Enqueue(
                [this, cmdType, payload = std::move(payload)] { return ExecuteCommand(cmdType, payload); });

            CommandQueue::Response response;
            try
            {
                response = pending.get();
            }
            catch (const std::future_error &)
            {
                break; // the queue was closed: the server is stopping
            }

            // The session follows the connection's last Hello: a refused one (wrong token or version) ends it
            const bool isHello = static_cast<CommandType>(cmdType) == CommandType::Hello;
            if (isHello)
            {
                helloAccepted = !response.empty() && response[0] == static_cast<uint8_t>(ResponseType::ServerInfo);
                sessionOpened = sessionOpened || helloAccepted;
            }

            if (!Send(clientSocket, response.data(), response.size()))
                break;

            // With an access token, a refused Hello also ends the connection, once its Error is sent
            if (gated && isHello && !helloAccepted)
            {
                closeUnauthorized("its Hello was refused");
                break;
            }
        }
    }

    std::vector<uint8_t> EditorServer::ExecuteCommand(uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        _response.clear();

        bool failed = false;
        std::string failure;
        try
        {
            // No socket: handlers only build the response, which SendResponse records in _response
            ProcessCommand(-1, commandType, payload);
        }
        catch (const std::exception &e)
        {
            failed = true;
            failure = e.what();
        }
        catch (...)
        {
            failed = true;
            failure = "unknown exception";
        }

        // The autosave an edit left due, written once the command is done, so it sees the scene as the command left it
        // (destroyed objects purged) even when the command then threw
        try
        {
            FlushAutosave();
        }
        catch (...)
        {
            // An autosave is never worth failing a command for
        }

        if (failed || _response.empty())
        {
            _response.clear();
            if (!failed)
            {
                failure = "the command produced no response";
            }
            Logger::Error(std::format("Command 0x{:X} failed: {}", commandType, failure));

            BufferWriter response;
            WriteError(response, "Command failed: " + failure);
            return {response.Data().begin(), response.Data().end()};
        }

        return std::exchange(_response, {});
    }

    bool EditorServer::IsViewportSizeValid(int32_t width, int32_t height)
    {
        return width > 0 && height > 0 && width <= MaxViewportDimension && height <= MaxViewportDimension;
    }

    std::string EditorServer::RenderTargetError(const int width, const int height)
    {
        return std::format("Couldn't create a {}x{} render target", width, height);
    }

    void EditorServer::ReadFrame(const Renderer::Common::IRenderer &renderer, const int width, const int height,
                                 std::vector<uint8_t> &pixels)
    {
        const size_t rowBytes = width > 0 ? static_cast<size_t>(width) * 4 : 0;
        const size_t rows = height > 0 ? static_cast<size_t>(height) : 0;
        // resize, not assign: every live backend writes the whole frame, so zeroing ~3.7 MB per 1280x720 frame
        // would be wasted. New bytes are zero, so a backend that writes nothing (Vulkan's stub) gives black.
        pixels.resize(rowBytes * rows);
        if (pixels.empty())
        {
            return;
        }

        // Every backend reads back RGBA, bottom row first (IRenderer::ReadFramebuffer); FrameData is top row first
        renderer.ReadFramebuffer(pixels.data(), width, height);
        Renderer::Common::FlipRows(pixels.data(), rowBytes, rows);

        // The viewport is opaque: alpha is whatever blending left in the target, which a client drawing the
        // pixels as an image would show as see-through
        for (size_t i = 3; i < pixels.size(); i += 4)
        {
            pixels[i] = 0xFF;
        }
    }

    std::optional<std::filesystem::path> EditorServer::ResolveSceneFile(const std::filesystem::path &scenesDirectory,
                                                                        const std::string &sceneName)
    {
        if (scenesDirectory.empty() || sceneName.empty() || sceneName.find('\0') != std::string::npos)
            return std::nullopt;

        std::filesystem::path relative(sceneName);
        // Rejects "C:\x", "/x", "\x" and "C:x" outright (only a relative path inside the directory is valid)
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
            return std::nullopt;
        // "dir/", "." and ".." name a directory, not a scene file
        const std::filesystem::path originalFileName = relative.filename();
        if (originalFileName.empty() || originalFileName == "." || originalFileName == "..")
            return std::nullopt;

        // Append the extension unless the name already ends with it (in any case), so "Level.1" works
        const std::string_view extension = SceneFileExtension;
        const bool hasExtension =
            sceneName.size() > extension.size() &&
            std::equal(extension.begin(), extension.end(), sceneName.end() - static_cast<std::ptrdiff_t>(extension.size()),
                       [](char a, char b)
                       {
                           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                       });
        if (!hasExtension)
            relative += SceneFileExtension;

        // The parent is canonicalized (symlinks and ".." resolved) and must be the directory or inside it.
        // The file name itself is not resolved, so a symlinked scene file is deleted as a link, never its target.
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(scenesDirectory, error);
        if (error)
            return std::nullopt;
        const std::filesystem::path parent = std::filesystem::weakly_canonical((root / relative).parent_path(), error);
        if (error)
            return std::nullopt;

        const std::filesystem::path fromRoot = parent.lexically_relative(root);
        if (fromRoot.empty() || *fromRoot.begin() == "..")
            return std::nullopt;

        return parent / relative.filename();
    }

    bool EditorServer::IsPolledCommand(uint8_t commandType)
    {
        switch (static_cast<CommandType>(commandType))
        {
        case CommandType::RenderFrame:        // every animation frame
        case CommandType::RenderFrameIfChanged: // every animation frame (mostly answered "not modified")
        case CommandType::SetEditorCamera:    // every animation frame of a camera drag
        case CommandType::GetEditorCamera:    // the scene view, with each frame, for its gizmos
        case CommandType::PickEntity:         // a click, or the mouse moving over the scene view
        case CommandType::GetEntityBounds:    // the selection box, whenever the selection or the scene changes
        case CommandType::GetPlayState:       // the play toolbar
        case CommandType::SendInput:          // the play viewport: every key and pointer event
        case CommandType::ListAssets:         // the asset panel, whenever assetsChanged arrives
        case CommandType::GetAssetInfo:       // the asset panel's details, for the selected asset
        case CommandType::ReadTextAsset:      // the script editor, whenever assetsChanged lists its file
        case CommandType::GetAudio:           // every ~25 ms while audio plays
        case CommandType::PollEvents:         // every ~100 ms; a line logged per poll would be an event every poll
        case CommandType::GetAllEntities:     // the hierarchy panel
        case CommandType::GetEntityTransform: // the inspector
        case CommandType::GetCameraPosition:  // the scene view
        case CommandType::GetEngineHealth:    // the status panel
        case CommandType::GetHierarchy:       // the hierarchy panel, whenever the scene revision moves
        case CommandType::GetEntity:          // the inspector, whenever the selected object changes
        case CommandType::GetComponent:       // the inspector, for the components it shows
        case CommandType::GetComponentTypes:  // the inspector, once per session (and after Hello)
        case CommandType::GetLuaFields:       // the inspector, for a script component it shows
        case CommandType::GetHistory:         // the Edit menu, whenever historyChanged arrives
        case CommandType::GetAutosave:        // after opening a scene
            return true;
        default:
            return false;
        }
    }

    void EditorServer::ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        // No generic "command issued" line: every log line is an event for the editor (PollEvents), and handlers log
        // what matters themselves. A polled command's handler never logs per call (see IsPolledCommand); failures are
        // logged by ExecuteCommand.
        auto cmd = static_cast<CommandType>(commandType);

        // A play host plays a snapshot: it never opens another scene or writes the project's files
        if (_playMode && IsEditOnlyCommand(commandType))
        {
            SendError(clientSocket, "Not available in a play host: it plays a snapshot and never changes the project's files");
            return;
        }

        switch (cmd)
        {
        case CommandType::Hello:
            HandleHello(clientSocket, payload);
            break;
        case CommandType::RenderFrame:
            HandleRenderFrame(clientSocket);
            break;
        case CommandType::SetViewportSize:
            HandleSetViewportSize(clientSocket, payload);
            break;
        case CommandType::RenderFrameIfChanged:
            HandleRenderFrameIfChanged(clientSocket, payload);
            break;
        case CommandType::SetEditorCamera:
            HandleSetEditorCamera(clientSocket, payload);
            break;
        case CommandType::GetEditorCamera:
            HandleGetEditorCamera(clientSocket);
            break;
        case CommandType::PickEntity:
            HandlePickEntity(clientSocket, payload);
            break;
        case CommandType::GetEntityBounds:
            HandleGetEntityBounds(clientSocket, payload);
            break;
        case CommandType::WritePlaySnapshot:
            HandleWritePlaySnapshot(clientSocket, payload);
            break;
        case CommandType::SetPaused:
            HandleSetPaused(clientSocket, payload);
            break;
        case CommandType::Step:
            HandleStep(clientSocket, payload);
            break;
        case CommandType::GetPlayState:
            HandleGetPlayState(clientSocket);
            break;
        case CommandType::SendInput:
            HandleSendInput(clientSocket, payload);
            break;
        case CommandType::GetAudio:
            HandleGetAudio(clientSocket);
            break;
        case CommandType::PollEvents:
            HandlePollEvents(clientSocket, payload);
            break;
        case CommandType::SetCameraPosition:
            HandleSetCameraPosition(clientSocket, payload);
            break;
        case CommandType::GetCameraPosition:
            HandleGetCameraPosition(clientSocket);
            break;
        case CommandType::CreateScene:
            HandleCreateScene(clientSocket, payload);
            break;
        case CommandType::LoadScene:
            HandleLoadScene(clientSocket, payload);
            break;
        case CommandType::SaveScene:
            HandleSaveScene(clientSocket);
            break;
        case CommandType::DeleteScene:
            HandleDeleteScene(clientSocket, payload);
            break;
        case CommandType::GetCurrentScene:
            HandleGetCurrentScene(clientSocket);
            break;
        case CommandType::OpenScene:
            HandleOpenScene(clientSocket, payload);
            break;
        case CommandType::SaveSceneToFile:
            HandleSaveSceneToFile(clientSocket, payload);
            break;
        case CommandType::NewScene:
            HandleNewScene(clientSocket, payload);
            break;
        case CommandType::GetHierarchy:
            HandleGetHierarchy(clientSocket);
            break;
        case CommandType::GetOpenScene:
            HandleGetOpenScene(clientSocket);
            break;
        case CommandType::GetProjectInfo:
            HandleGetProjectInfo(clientSocket);
            break;
        case CommandType::SetProjectSettings:
            HandleSetProjectSettings(clientSocket, payload);
            break;
        case CommandType::SetStartupScene:
            HandleSetStartupScene(clientSocket, payload);
            break;
        case CommandType::CreateEntity:
            HandleCreateEntity(clientSocket, payload);
            break;
        case CommandType::DestroyEntity:
            HandleDestroyEntity(clientSocket, payload);
            break;
        case CommandType::SetEntityTransform:
            HandleSetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetEntityTransform:
            HandleGetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetAllEntities:
            HandleGetAllEntities(clientSocket);
            break;
        case CommandType::CreateEntityEx:
            HandleCreateEntityEx(clientSocket, payload);
            break;
        case CommandType::SetEntityParent:
            HandleSetEntityParent(clientSocket, payload);
            break;
        case CommandType::SetEntityProperties:
            HandleSetEntityProperties(clientSocket, payload);
            break;
        case CommandType::DuplicateEntity:
            HandleDuplicateEntity(clientSocket, payload);
            break;
        case CommandType::GetEntity:
            HandleGetEntity(clientSocket, payload);
            break;
        case CommandType::SetLocalTransform:
            HandleSetLocalTransform(clientSocket, payload);
            break;
        case CommandType::GetComponentTypes:
            HandleGetComponentTypes(clientSocket);
            break;
        case CommandType::AddComponent:
            HandleAddComponent(clientSocket, payload);
            break;
        case CommandType::RemoveComponent:
            HandleRemoveComponent(clientSocket, payload);
            break;
        case CommandType::SetComponentFields:
            HandleSetComponentFields(clientSocket, payload);
            break;
        case CommandType::GetComponent:
            HandleGetComponent(clientSocket, payload);
            break;
        case CommandType::GetLuaFields:
            HandleGetLuaFields(clientSocket, payload);
            break;
        case CommandType::CreateScript:
            HandleCreateScript(clientSocket, payload);
            break;
        case CommandType::RescanAssets:
            HandleRescanAssets(clientSocket);
            break;
        case CommandType::ListAssets:
            HandleListAssets(clientSocket, payload);
            break;
        case CommandType::GetAssetInfo:
            HandleGetAssetInfo(clientSocket, payload);
            break;
        case CommandType::SetImportSettings:
            HandleSetImportSettings(clientSocket, payload);
            break;
        case CommandType::ReadTextAsset:
            HandleReadTextAsset(clientSocket, payload);
            break;
        case CommandType::WriteTextAsset:
            HandleWriteTextAsset(clientSocket, payload);
            break;
        case CommandType::CreateScriptAsset:
            HandleCreateScriptAsset(clientSocket, payload);
            break;
        case CommandType::CreateFolder:
            HandleCreateFolder(clientSocket, payload);
            break;
        case CommandType::GetEngineHealth:
            HandleGetEngineHealth(clientSocket);
            break;
        case CommandType::Undo:
            HandleUndo(clientSocket);
            break;
        case CommandType::Redo:
            HandleRedo(clientSocket);
            break;
        case CommandType::BeginEditGroup:
            HandleBeginEditGroup(clientSocket, payload);
            break;
        case CommandType::EndEditGroup:
            HandleEndEditGroup(clientSocket);
            break;
        case CommandType::GetHistory:
            HandleGetHistory(clientSocket);
            break;
        case CommandType::GetAutosave:
            HandleGetAutosave(clientSocket);
            break;
        case CommandType::RestoreAutosave:
            HandleRestoreAutosave(clientSocket);
            break;
        case CommandType::DiscardAutosave:
            HandleDiscardAutosave(clientSocket);
            break;
        default:
            Logger::Warn("Unknown command: " + std::to_string(commandType));
            BufferWriter response;
            WriteError(response, "Unknown command");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            break;
        }
    }

    void EditorServer::HandleHello(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const HelloCmd cmd = HelloCmd::Deserialize(reader);

        // Client-supplied text only reaches the log, or the Error, sanitised and shortened; the token never does
        const std::string clientName = SanitizeForLog(cmd.clientName);
        const std::string clientVersionText = SanitizeForLog(cmd.protocolVersion, 40);
        BufferWriter response;

        // The token first, so a client without it learns nothing else
        if (!_accessToken.empty() && !TokensMatch(_accessToken, cmd.token))
        {
            Logger::Warn("Refused Hello from editor client '" + clientName + "': wrong access token");
            WriteError(response, "Invalid access token");
            SendResponse(clientSocket, response.Release());
            return;
        }

        const auto clientVersion = ParseProtocolVersion(cmd.protocolVersion);
        const auto serverVersion = ParseProtocolVersion(ProtocolVersion);
        if (!clientVersion.has_value() || !serverVersion.has_value() ||
            clientVersion->majorVersion != serverVersion->majorVersion)
        {
            const std::string message =
                clientVersion.has_value()
                    ? std::format("Protocol version mismatch: this server speaks {}, the client {} (the major versions "
                                  "must match)", ProtocolVersion, clientVersionText)
                    : std::format("Invalid protocol version '{}' (expected major.minor.patch; this server speaks {})",
                                  clientVersionText, ProtocolVersion);
            Logger::Warn("Refused Hello from editor client '" + clientName + "': " + message);
            WriteError(response, message);
            SendResponse(clientSocket, response.Release());
            return;
        }

        if (clientVersion->minorVersion != serverVersion->minorVersion)
        {
            // Compatible, but one side has commands the other doesn't: those fail with "Unknown command"
            Logger::Warn(std::format("Editor client '{}' said Hello with protocol {}, this server speaks {}: commands "
                                     "only one of them knows will fail", clientName, clientVersionText, ProtocolVersion));
        }
        else
        {
            Logger::Info(std::format("Editor client '{}' said Hello (protocol {})", clientName, clientVersionText));
        }
        // A new session: a group the last one left open is ended, and the keys it held (a play host's) go up
        CloseEditGroups();
        _keys.ReleaseAll();
        // A project is loaded when the host opened one (--project, SetProject)
        WriteServerInfo(response, ProtocolVersion, EngineVersion(), Capabilities(), HasProject());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleRenderFrame(int clientSocket)
    {
        auto &app = Application::GetInstance();
        auto &window = app.GetWindow();

        auto *renderer = window.GetRenderer();
        if (!renderer)
        {
            BufferWriter response;
            WriteError(response, "No renderer available (see GetEngineHealth)");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // Render at the viewport size (validated by SetViewportSize). SetViewportSize already applied it; this
        // covers the default size, before any SetViewportSize, does nothing when the size is unchanged, and
        // retries a size the renderer failed to make before. A frame of another size is never sent.
        if (!window.SetRenderSize(_viewportWidth, _viewportHeight))
        {
            BufferWriter response;
            WriteError(response, RenderTargetError(_viewportWidth, _viewportHeight));
            SendResponse(clientSocket, response.Release());
            return;
        }
        // This is the game camera's picture, in the buffer that holds the editor view's frame too: the editor view's
        // frame has to be rendered again to be sent (its revision is still current: nothing about the editor view
        // changed). Said first, so a render or read that throws can't leave a half-written buffer marked valid.
        _frames.InvalidateBuffer();
        if (_playMode)
        {
            // The running game from its main camera: no window events, no clock (the game's frames own both)
            app.RenderGameFrame();
        }
        else
        {
            app.RenderEditorFrame();
        }

        // RGBA, top row first, whatever the backend
        ReadFrame(*renderer, _viewportWidth, _viewportHeight, _frameBuffer);

        BufferWriter response;
        WriteFrameData(response,
                       static_cast<uint32_t>(_viewportWidth),
                       static_cast<uint32_t>(_viewportHeight),
                       _frameBuffer);

        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetAudio(int clientSocket)
    {
        auto &audio = Audio::AudioSystem::Instance();
        BufferWriter response;
        if (!audio.IsLoopback())
        {
            WriteError(response, "No audio stream: audio isn't running on a loopback device (see GetEngineHealth)");
            SendResponse(clientSocket, response.Release());
            return;
        }

        // Everything mixed since the previous GetAudio (at most AudioSystem::StreamBufferMilliseconds of it)
        const Audio::LoopbackFormat &format = audio.GetLoopbackFormat();
        const Audio::StreamedAudio streamed = audio.TakeStreamedAudio();
        WriteAudioSamples(response, format.sampleRate, format.channels, std::string{Audio::ToString(format.sampleFormat)},
                          streamed.frameCount, streamed.droppedFrames, streamed.samples);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandlePollEvents(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const PollEventsCmd cmd = PollEventsCmd::Deserialize(reader);

        // Nothing here may log: every logged line is an event, so a poll that logged would always find one more. (Only
        // a failure, such as a malformed payload, is logged, by ExecuteCommand, as for every polled command.)
        EventBatch batch = _events.Read(cmd.afterSeq, cmd.maxEvents, cmd.epoch);
        BufferWriter response;
        WriteEvents(response, batch.epoch, batch.nextSeq, batch.dropped, nlohmann::json(std::move(batch.events)));
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetViewportSizeCmd::Deserialize(reader);

        if (!IsViewportSizeValid(cmd.width, cmd.height))
        {
            BufferWriter response;
            WriteError(response, std::format("Invalid viewport size {}x{} (each must be 1 to {})",
                                             cmd.width, cmd.height, MaxViewportDimension));
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // A new size is a new picture (a same-size request, which the client sends on every window resize event, isn't)
        const bool viewportChanged = cmd.width != _viewportWidth || cmd.height != _viewportHeight;
        _viewportWidth = cmd.width;
        _viewportHeight = cmd.height;
        if (viewportChanged)
        {
            NoteViewChanged();
        }

        // The renderer renders at this size from the next frame (an offscreen target on OpenGL, the CPU buffer's
        // size on the software renderer), so frames are neither cropped nor resampled; this also sets the camera's
        // aspect. Without a renderer only the aspect changes. The size is kept even when the renderer can't make
        // the target, so RenderFrame retries it (and answers Error until it works).
        auto &app = Application::GetInstance();
        auto &window = app.GetWindow();
        if (!window.GetRenderer())
        {
            app.OnWindowResize(_viewportWidth, _viewportHeight);
        }
        else if (!window.SetRenderSize(_viewportWidth, _viewportHeight))
        {
            BufferWriter response;
            WriteError(response, RenderTargetError(_viewportWidth, _viewportHeight));
            SendResponse(clientSocket, response.Release());
            return;
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::NoteSceneChanged()
    {
        NoteViewChanged();
        // Reported: the lazy look at the scene (ObserveScene) mustn't count this change again, or the frame would be
        // two revisions on from the last, and frameChanged would announce a revision no frame has
        _frames.RecordScene(SceneManager::GetCurScene(), _sceneRevision);
    }

    void EditorServer::NoteViewChanged()
    {
        if (_frames.MarkChanged())
        {
            _events.Push("frameChanged", nlohmann::json{{"revision", _frames.Revision()}});
        }
    }

    void EditorServer::ObserveScene()
    {
        if (_frames.ObserveScene(SceneManager::GetCurScene(), _sceneRevision))
        {
            _events.Push("frameChanged", nlohmann::json{{"revision", _frames.Revision()}});
        }
    }

    std::expected<void, std::string> EditorServer::SetEditorCameraState(const EditorCameraState &camera)
    {
        EditorCameraCheck checked = CheckEditorCamera(camera);
        if (!checked.state)
        {
            return std::unexpected(std::move(checked.error));
        }
        // The camera it has already: no change, so no new frame
        if (*checked.state == _editorCamera)
        {
            return {};
        }
        _editorCamera = *checked.state;
        NoteViewChanged();
        return {};
    }

    bool EditorServer::RenderEditorView(std::string &error)
    {
        auto &app = Application::GetInstance();
        auto &window = app.GetWindow();

        auto *renderer = window.GetRenderer();
        if (!renderer)
        {
            error = "No renderer available (see GetEngineHealth)";
            return false;
        }
        // As RenderFrame: the viewport size, retried when the renderer failed to make it before
        if (!window.SetRenderSize(_viewportWidth, _viewportHeight))
        {
            error = RenderTargetError(_viewportWidth, _viewportHeight);
            return false;
        }

        // The open scene, drawn from the editor camera (not the scene's, nor the game's main camera), without simulating
        const Camera camera = _editorCamera.ToCamera(GetViewportAspect());
        app.RenderEditorFrame(camera);
        ReadFrame(*renderer, _viewportWidth, _viewportHeight, _frameBuffer);

        _frames.MarkRendered();
        ++_editorFramesRendered;
        return true;
    }

    void EditorServer::HandleRenderFrameIfChanged(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const RenderFrameIfChangedCmd cmd = RenderFrameIfChangedCmd::Deserialize(reader);

        if (_playMode)
        {
            // The game changes every frame and its picture has no revision (and the editor view's render would move the
            // game's clock): a play viewport draws with RenderFrame
            SendError(clientSocket, "A play host draws the running game with RenderFrame, not the editor view");
            return;
        }

        // A scene change no handler reported still moves the revision on
        ObserveScene();

        BufferWriter response;
        const auto width = static_cast<uint32_t>(_viewportWidth);
        const auto height = static_cast<uint32_t>(_viewportHeight);
        if (_frames.IsCurrent(cmd.sinceRevision))
        {
            // The client holds the current picture: nothing is rendered, copied or logged
            WriteFrameUpdate(response, _frames.Revision(), false, width, height, {});
            SendResponse(clientSocket, response.Release());
            return;
        }

        // Another client may have asked for this picture already: it is still in the buffer unless RenderFrame (the game
        // camera's picture) has used the buffer since
        if (!_frames.HasCurrentFrame())
        {
            std::string error;
            if (!RenderEditorView(error))
            {
                // The client was told a frame was waiting and none came: the next change must tell it again
                _frames.RearmAnnouncement();
                SendError(clientSocket, error);
                return;
            }
        }
        WriteFrameUpdate(response, _frames.Revision(), true, width, height, _frameBuffer);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetEditorCamera(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetEditorCameraCmd cmd = SetEditorCameraCmd::Deserialize(reader);

        EditorCameraState camera;
        camera.position = cmd.position;
        camera.rotation = cmd.rotation;
        camera.fovY = cmd.fovY;
        camera.orthographic = cmd.orthographic;
        camera.orthoSize = cmd.orthoSize;
        camera.nearPlane = cmd.nearPlane;
        camera.farPlane = cmd.farPlane;

        // Never logs, not even a refusal's reason: the Error response carries it (a polled-rate command)
        if (const auto set = SetEditorCameraState(camera); !set)
        {
            SendError(clientSocket, set.error());
            return;
        }
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEditorCamera(int clientSocket)
    {
        const Camera camera = _editorCamera.ToCamera(GetViewportAspect());
        BufferWriter response;
        WriteEditorCamera(response, _editorCamera.position, _editorCamera.rotation, _editorCamera.fovY,
                          _editorCamera.orthographic, _editorCamera.orthoSize, _editorCamera.nearPlane,
                          _editorCamera.farPlane, camera.GetViewMatrix(), camera.GetProjectionMatrix());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandlePickEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const PickEntityCmd cmd = PickEntityCmd::Deserialize(reader);

        // Never logs (a polled-rate command): a refusal is the Error response
        if (!std::isfinite(cmd.x) || !std::isfinite(cmd.y))
        {
            SendError(clientSocket, "x and y must be finite numbers");
            return;
        }
        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        std::string entityId;
        Math::Vector3 point{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;
        // A point outside the viewport is over nothing (the same rule as the UI's window hit test)
        if (cmd.x >= 0.0f && cmd.y >= 0.0f && cmd.x < static_cast<float>(_viewportWidth) &&
            cmd.y < static_cast<float>(_viewportHeight))
        {
            // The ray the editor view's frames are seen through: the same camera, at the viewport size
            const Camera camera = _editorCamera.ToCamera(GetViewportAspect());
            float nearToFar = 0.0f;
            const Math::Ray ray = camera.ScreenPointToRay(Math::Vector2{cmd.x, cmd.y},
                                                          Vector2i{_viewportWidth, _viewportHeight}, &nearToFar);
            Picking::PickOptions options;
            options.includeInactive = cmd.includeInactive;
            if (std::isfinite(nearToFar) && nearToFar > 0.0f)
            {
                options.maxDistance = nearToFar; // nothing beyond the far plane is drawn
            }
            const Picking::PickHit hit = Picking::PickGameObject(*scene, ray, options);
            if (hit.gameObject != nullptr)
            {
                entityId = hit.gameObject->GetUUID().ToString();
                point = hit.point;
                distance = hit.distance;
            }
        }

        BufferWriter response;
        WritePickResult(response, entityId, point, distance);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEntityBounds(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetEntityBoundsCmd cmd = GetEntityBoundsCmd::Deserialize(reader);

        // Never logs (a polled-rate command): a refusal is the Error response
        if (!cmd.entityIds.is_array())
        {
            SendError(clientSocket, "entityIds must be a JSON array of UUID strings");
            return;
        }
        if (cmd.entityIds.size() > MaxBoundsEntityIds)
        {
            SendError(clientSocket, std::format("entityIds has {} ids; at most {} are answered at once",
                                                cmd.entityIds.size(), MaxBoundsEntityIds));
            return;
        }
        for (const nlohmann::json &id : cmd.entityIds)
        {
            if (!id.is_string())
            {
                SendError(clientSocket, "entityIds must be a JSON array of UUID strings");
                return;
            }
        }
        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        nlohmann::json bounds = nlohmann::json::array();
        std::unordered_set<std::string> answered;
        for (const nlohmann::json &id : cmd.entityIds)
        {
            // An id that names no object (a stale one) is left out
            const std::shared_ptr<GameObject> entity = FindEntity(*scene, id.get<std::string>());
            if (entity == nullptr)
            {
                continue;
            }
            std::string canonical = entity->GetUUID().ToString();
            if (!answered.insert(canonical).second)
            {
                continue;
            }
            if (const std::optional<BoundingBox> box = Picking::GetGameObjectBounds(*entity))
            {
                bounds.push_back(nlohmann::json{{"id", std::move(canonical)}, {"min", Vec3Json(box->min)},
                                                {"max", Vec3Json(box->max)}});
            }
        }

        BufferWriter response;
        WriteBounds(response, bounds);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetCameraPositionCmd::Deserialize(reader);

        auto *camera = Application::GetInstance().GetMainCamera();
        if (camera)
            camera->SetPosition({cmd.x, cmd.y, cmd.z});

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCameraPosition(int clientSocket)
    {
        auto *camera = Application::GetInstance().GetMainCamera();

        BufferWriter response;
        if (camera)
        {
            auto pos = camera->GetPosition();
            WriteCameraPosition(response, pos[0], pos[1], pos[2]);
        }
        else
        {
            WriteError(response, "No camera");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleCreateScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateSceneCmd::Deserialize(reader);

        BufferWriter response;

        // AddScene replaces a stored scene with the same name, which would wipe that scene's data
        if (SceneManager::HasScene(cmd.name))
        {
            WriteError(response, "Scene already exists: " + cmd.name);
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        try
        {
            auto newScene = Scene::Create(cmd.name);

            // Serialize it before moving
            nlohmann::json sceneJson = newScene->Serialize();
            std::string sceneJsonString = sceneJson.dump();

            SceneManager::AddScene(std::move(newScene), false);

            WriteSceneData(response, sceneJsonString);
            Logger::Info("Created scene: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to create scene: " + std::string(e.what()));
            WriteError(response, std::string("Failed to create scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string sceneJsonString = LoadSceneCmd::Deserialize(reader).sceneJson;

        Logger::Info("Loading scene from scene data: " + sceneJsonString.substr(0, 200));

        // Non-throwing parse: the JSON comes from the client
        nlohmann::json sceneJson = nlohmann::json::parse(sceneJsonString, nullptr, false);
        if (sceneJson.is_discarded())
        {
            BufferWriter response;
            WriteError(response, "The scene passed is not valid JSON");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        auto scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "The scene passed was invalid or corrupt");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // The scene that was open is left: what it hadn't saved is dropped, and so is its autosave
        DiscardWrittenAutosave();

        if (int sceneIndex = SceneManager::GetSceneIndex(scene->sceneName); sceneIndex != -1)
        {
            SceneManager::UpdateScene(sceneIndex, sceneJson);
            SceneManager::LoadScene(sceneIndex);
        }
        else
        {
            SceneManager::AddScene(sceneJson);
            SceneManager::LoadScene(scene->sceneName);
        }
        SceneManager::ProcessAnyPendingSceneChange();

        // The scene came from the client, not a file: it has none (until SaveSceneToFile names one), and it is
        // unsaved
        _openScenePath.clear();
        _openScene = SceneManager::GetCurScene();
        if (SceneManager::GetCurScene() != nullptr)
        {
            // Opened for editing, like every scene this host loads
            SceneManager::GetCurScene()->SetEditMode(true);
        }
        MarkSceneChanged({}, true);
        // Another scene: nothing done to the last can be undone, and no state of this one is saved
        _history.Clear();
        ResetAutosaveState();
        PushHistoryChanged();

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSaveScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            Logger::Warn("No scene loaded to save");
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});

            return;
        }
        nlohmann::json sceneJson = SceneManager::GetCurSceneRef().Serialize();
        SceneManager::UpdateScene(SceneManager::GetCurSceneIndex(), sceneJson);

        // dump(): passing the json itself converted it to a string, which throws for an object
        WriteSceneData(response, sceneJson.dump());
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDeleteScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = DeleteSceneCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            // The name comes from the client: only ever delete a scene file inside the scenes directory.
            // Checking then removing is racy, but exploiting that needs local write access to scenes/,
            // which is accepted for a dev tool.
            const auto sceneFile = ResolveSceneFile(_scenesDirectory, cmd.sceneName);
            const Scene *loadedScene = SceneManager::GetCurScene();
            if (_scenesDirectory.empty())
            {
                WriteError(response, "No scenes directory configured (start the host with --project)");
            }
            else if (!sceneFile.has_value())
            {
                Logger::Warn("Rejected DeleteScene outside the scenes directory: " + cmd.sceneName);
                WriteError(response, "Not a scene file in the project's scenes directory: " + cmd.sceneName);
            }
            else if (loadedScene != nullptr && sceneFile->stem() == std::filesystem::path(loadedScene->sceneName))
            {
                // As SceneManager::DeleteScene refuses to delete the loaded scene
                WriteError(response, "Can't delete the loaded scene's file: " + cmd.sceneName);
            }
            else if (std::filesystem::is_regular_file(*sceneFile))
            {
                // Logged with the client's (UTF-8) name, before removing: converting the path back to a
                // narrow string can throw for names the code page can't represent.
                // Only the file is removed. Scenes in SceneManager aren't tied to files (the editor loads
                // them by sending JSON), so a stored scene that happens to share the name is left alone.
                Logger::Info("Deleting scene file: " + cmd.sceneName);
                std::filesystem::remove(*sceneFile);
                WriteOk(response);
            }
            else
            {
                WriteError(response, "Scene file not found: " + cmd.sceneName);
            }
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to delete scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCurrentScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        try
        {
            Scene &scene = SceneManager::GetCurSceneRef();
            nlohmann::json sceneJson = scene.Serialize();

            WriteSceneData(response, sceneJson.dump());
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to get current scene: " + std::string(e.what()));
            WriteError(response, "Failed to get current scene: " + std::string(e.what()));
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
    }

    void EditorServer::HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateEntityCmd::Deserialize(reader);

        BufferWriter response;

        // Create entity in current scene
        if (SceneManager::GetCurScene() != nullptr)
        {
            auto gameObject = GameObject::Create(cmd.name);
            SceneManager::GetCurSceneRef().AddRootGameObject(gameObject);
            MarkSceneChanged({gameObject->GetUUID().ToString()});
            RecordEdit(Labelled("Create", gameObject->GetName()), CreatedOp(*gameObject));
            WriteEntityCreated(response, gameObject->GetUUID().ToString());
        }
        else
        {
            Logger::Warn("No scene present");
            WriteError(response, "No scene present");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string entityId = DestroyEntityCmd::Deserialize(reader).entityId;

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        bool entityDestroyed = false;
        if (const auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            if (const auto foundGameObject = scene->FindGameObjectByUUID(uuid.value()))
            {
                // A real destroy (OnDisable/OnDestroy, children, components), not just detaching the root.
                // The editor host never runs game frames, so the end-of-frame teardown is run here instead;
                // it flushes every queued destroy in the scene and runs component (incl. Lua) callbacks.
                // If a callback throws, ExecuteCommand reports it as an Error (some objects may then stay
                // marked but unpurged) and later commands keep working.
                // Its whole subtree goes with it: the ids a client drops
                std::vector<std::string> destroyedIds = SubtreeIds(*foundGameObject);
                // What undoing needs: the scene as it is, since destroying leaves references elsewhere dangling, and
                // putting the objects back alone wouldn't point them at their targets again (a load does, by UUID)
                std::shared_ptr<const std::string> snapshot;
                if (scene->IsEditMode())
                {
                    snapshot = std::make_shared<const std::string>(SnapshotScene(*scene));
                    if (_history.InGroup() && _history.GroupBytes() > 0 && _history.GroupBytes() + snapshot->size() > _history.GetMaxBytes())
                    {
                        SendError(clientSocket, "The open edit group already holds too much to undo: end it first");
                        return;
                    }
                }
                const std::string destroyedId = foundGameObject->GetUUID().ToString();
                const std::string label = Labelled("Delete", foundGameObject->GetName());
                if (scene->DestroyGameObject(foundGameObject))
                {
                    // Marked first: a callback that throws still leaves the scene changed
                    MarkSceneChanged(destroyedIds);
                    if (scene->IsEditMode())
                    {
                        // The objects are still there until they are purged: nothing may point at them afterwards.
                        // The holders of those references changed too: a second event lists them.
                        std::vector<std::string> holders = ForgetDestroyed(*scene, *foundGameObject);
                        const std::unordered_set<std::string> listed(destroyedIds.begin(), destroyedIds.end());
                        std::erase_if(holders, [&listed](const std::string &id) { return listed.contains(id); });
                        if (!holders.empty())
                        {
                            PushSceneChanged(std::move(holders));
                        }
                    }
                    entityDestroyed = true;

                    // Recorded before the purge, which can throw (a callback): the scene has changed either way. The
                    // autosave it makes due is written once the command is over (ExecuteCommand), objects purged.
                    if (snapshot != nullptr)
                    {
                        EditOp op;
                        op.undo = [this, snapshot]() -> EditOutcome { return RestoreSceneSnapshot(*snapshot); };
                        op.redo = [destroyedId]() -> EditOutcome { return DestroySubtree(destroyedId); };
                        op.undoBytes = snapshot->size();
                        RecordEdit(label, std::move(op));
                    }
                    scene->ProcessDestroyed();
                }
            }
        }

        BufferWriter response;
        if (entityDestroyed)
        {
            WriteOk(response);
        }
        else
        {
            WriteError(response, "No game object found with entity id " + entityId);
        }
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetEntityTransformCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // Apply transform to entity
        bool applied = false;
        if (auto uuid = Math::UUID::FromString(cmd.entityId); uuid.has_value())
        {
            auto entity = scene->FindGameObjectByUUID(uuid.value());
            if (entity != nullptr && entity->HasPositionable())
            {
                const LocalTransformState before = LocalTransformOf(*entity);
                entity->GetPositionable()->SetPositionAndRotation(
                    cmd.position,
                    Math::Quaternion::FromEulerAngles(cmd.rotation)
                );
                entity->GetPositionable()->SetScale(cmd.scale);
                applied = true;
                // The same transform again changes nothing: no revision, no step (as SetLocalTransform)
                if (const LocalTransformState after = LocalTransformOf(*entity); !after.SameAs(before))
                {
                    // The children's world matrices moved with it
                    MarkSceneChanged(SubtreeIds(*entity));
                    RecordEdit(Labelled("Transform", entity->GetName()),
                               TransformOp(entity->GetUUID().ToString(), before, after));
                }
            }
        }

        BufferWriter response;
        if (applied)
            WriteOk(response);
        else
            WriteError(response, "Entity not found (or has no transform): " + cmd.entityId);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetAllEntities(int clientSocket)
    {
        BufferWriter response;

        // No scene: an empty list
        std::vector<EntityInfo> entities;
        if (SceneManager::GetCurScene() != nullptr)
        {
            for (const auto &go : SceneManager::GetCurSceneRef().GetAllGameObjects())
            {
                entities.push_back({go->GetUUID().ToString(), go->GetName()});
            }
        }

        WriteEntityList(response, entities);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Play mode (#82, E9) ====================

    namespace
    {
        constexpr const char *NotAPlayHostError =
            "Not a play host: start the host with --play <snapshot file> (WritePlaySnapshot writes one)";

        /// One SendInput event, checked
        struct InputAction
        {
            enum class Kind
            {
                Key,
                MouseButton,
                Pointer,
                Scroll,
                ReleaseAll
            };
            Kind kind = Kind::ReleaseAll;
            Input::Key key = Input::Key::Unknown;
            Input::MouseButton button = Input::MouseButton::Left;
            bool down = false;
            float x = 0.0f;
            float y = 0.0f;
        };

        /// A number member of an event: present, a JSON number, and finite
        std::optional<float> FiniteNumber(const nlohmann::json &event, const char *member)
        {
            const auto found = event.find(member);
            if (found == event.end() || !found->is_number())
            {
                return std::nullopt;
            }
            const double value = found->get<double>();
            if (!std::isfinite(value) || std::abs(value) > 1.0e9)
            {
                return std::nullopt;
            }
            return static_cast<float>(value);
        }

        /// SendInput's whole batch, checked before anything is applied; an Error message names the event
        std::expected<std::vector<InputAction>, std::string> ParseInputEvents(const nlohmann::json &events)
        {
            if (!events.is_array())
            {
                return std::unexpected("events must be a JSON array of InputEvent objects");
            }
            if (events.size() > EditorServer::MaxInputEvents)
            {
                return std::unexpected(std::format("At most {} input events at once, not {}",
                                                   EditorServer::MaxInputEvents, events.size()));
            }

            std::vector<InputAction> actions;
            actions.reserve(events.size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                const nlohmann::json &event = events[i];
                const auto bad = [i](const std::string &why)
                {
                    return std::unexpected(std::format("events[{}]: {}", i, why));
                };
                if (!event.is_object() || !event.contains("type") || !event.at("type").is_string())
                {
                    return bad("not an object with a string \"type\"");
                }
                const std::string type = event.at("type").get<std::string>();
                InputAction action;
                if (type == "key" || type == "mouseButton")
                {
                    const bool isKey = type == "key";
                    const char *nameMember = isKey ? "key" : "button";
                    if (!event.contains(nameMember) || !event.at(nameMember).is_string())
                    {
                        return bad(std::format("{} needs a string \"{}\"", type, nameMember));
                    }
                    if (!event.contains("down") || !event.at("down").is_boolean())
                    {
                        return bad(type + " needs a boolean \"down\"");
                    }
                    const std::string name = event.at(nameMember).get<std::string>();
                    action.down = event.at("down").get<bool>();
                    if (isKey)
                    {
                        action.kind = InputAction::Kind::Key;
                        action.key = event.at(nameMember).get<Input::Key>();
                        // An unknown name reads as the enum's first entry: only a name that comes back is a key
                        if (action.key == Input::Key::Unknown || nlohmann::json(action.key).get<std::string>() != name)
                        {
                            return bad("unknown key \"" + EditorServer::SanitizeForLog(name) + "\"");
                        }
                    }
                    else
                    {
                        action.kind = InputAction::Kind::MouseButton;
                        action.button = event.at(nameMember).get<Input::MouseButton>();
                        if (nlohmann::json(action.button).get<std::string>() != name)
                        {
                            return bad("unknown mouse button \"" + EditorServer::SanitizeForLog(name) + "\"");
                        }
                    }
                }
                else if (type == "pointer" || type == "scroll")
                {
                    const std::optional<float> x = FiniteNumber(event, "x");
                    const std::optional<float> y = FiniteNumber(event, "y");
                    if (!x || !y)
                    {
                        return bad(type + " needs finite numbers \"x\" and \"y\"");
                    }
                    action.kind = type == "pointer" ? InputAction::Kind::Pointer : InputAction::Kind::Scroll;
                    action.x = *x;
                    action.y = *y;
                }
                else if (type == "releaseAll")
                {
                    action.kind = InputAction::Kind::ReleaseAll;
                }
                else
                {
                    return bad("unknown type \"" + EditorServer::SanitizeForLog(type) + "\"");
                }
                actions.push_back(action);
            }
            return actions;
        }

        /// 32-bit FNV-1a of text, as 8 hex digits: the same on every platform, unlike std::hash
        std::string ShortHash(std::string_view text)
        {
            uint32_t hash = 2166136261u;
            for (const char c : text)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= 16777619u;
            }
            return std::format("{:08x}", hash);
        }

        /// A scene's name as a file name: letters, digits, dot, dash, underscore and space stay, the rest becomes '_'.
        /// A name Windows reserves for a device (CON, NUL, COM1...), with or without an extension, gets a '_' in front.
        std::string SafeSnapshotName(std::string_view name)
        {
            std::string safe;
            for (const char c : name)
            {
                const bool keep = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' ||
                                  c == '_' || c == ' ';
                safe.push_back(keep ? c : '_');
                if (safe.size() >= 80)
                {
                    break;
                }
            }
            // No leading or trailing dots and spaces (Windows strips them), and not empty
            while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
            {
                safe.pop_back();
            }
            while (!safe.empty() && (safe.front() == '.' || safe.front() == ' '))
            {
                safe.erase(safe.begin());
            }
            if (safe.empty())
            {
                return "scene";
            }
            std::string base = safe.substr(0, safe.find('.'));
            while (!base.empty() && base.back() == ' ')
            {
                base.pop_back();
            }
            for (char &c : base)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            const bool numbered = base.size() == 4 && base[3] >= '1' && base[3] <= '9' &&
                                  (base.starts_with("COM") || base.starts_with("LPT"));
            if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || numbered)
            {
                safe.insert(safe.begin(), '_');
            }
            return safe;
        }
    }

    bool EditorServer::IsEditOnlyCommand(const uint8_t commandType)
    {
        switch (static_cast<CommandType>(commandType))
        {
        case CommandType::OpenScene:
        case CommandType::NewScene:
        case CommandType::SaveSceneToFile:
        case CommandType::DeleteScene:
        case CommandType::SetProjectSettings:
        case CommandType::SetStartupScene:
        case CommandType::RestoreAutosave:
        case CommandType::DiscardAutosave:
        case CommandType::LoadScene:
        // E8's: they write files under assets/ or .import/
        case CommandType::SetImportSettings:
        case CommandType::WriteTextAsset:
        case CommandType::CreateScriptAsset:
        case CommandType::CreateFolder:
            return true;
        default:
            return false;
        }
    }

    std::expected<std::filesystem::path, std::string> EditorServer::WritePlaySnapshot(const std::string &scenePath)
    {
        if (_playMode)
        {
            return std::unexpected("A play host plays a snapshot; the host that edits writes them");
        }
        if (!_project)
        {
            return std::unexpected("No project: play snapshots go in the project's .n2 folder (start the host with --project)");
        }

        // The scene to play: the open one as it is in memory (no file read, so unsaved edits play), or another one's file
        bool useOpenScene = scenePath.empty();
        std::optional<ResolvedScenePath> resolved;
        if (!scenePath.empty())
        {
            auto checked = ResolveScenePath(_project->root / "assets", scenePath);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            const std::string open = OpenScenePath();
            if (!open.empty() && checked->resourcePath.ToString() == open)
            {
                useOpenScene = true;
            }
            else
            {
                resolved = std::move(*checked);
            }
        }

        nlohmann::json sceneJson;
        std::string name;
        // What tells two scenes of one name apart: the scene's file (empty for one that has none)
        std::string sceneKey;
        if (useOpenScene)
        {
            const Scene *scene = SceneManager::GetCurScene();
            if (scene == nullptr)
            {
                return std::unexpected("No scene loaded");
            }
            sceneJson = scene->Serialize();
            name = scene->sceneName;
            sceneKey = OpenScenePath();
        }
        else
        {
            const std::string resourcePath = resolved->resourcePath.ToString();
            std::error_code error;
            if (!std::filesystem::is_regular_file(resolved->file, error))
            {
                return std::unexpected("Scene file not found: " + SanitizeForLog(resourcePath));
            }
            auto text = ReadTextFile(resolved->file);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            sceneJson = nlohmann::json::parse(*text, nullptr, false);
            if (sceneJson.is_discarded())
            {
                return std::unexpected("Not valid JSON: " + SanitizeForLog(resourcePath));
            }
            // It must build as a scene (edit mode: built, never attached), or the play host would refuse it
            if (Scene::FromJSON(sceneJson, true) == nullptr)
            {
                return std::unexpected("Not a valid scene (see the log): " + SanitizeForLog(resourcePath));
            }
            name = resolved->resourcePath.GetStem();
            sceneKey = resolved->resourcePath.ToString();
        }

        const std::filesystem::path file = _project->root / ".n2" / "play" / (SafeSnapshotName(name) + "-" + ShortHash(sceneKey) + ".scene");
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (auto written = IO::WriteTextFileAtomically(file, SceneFileText(sceneJson)); !written)
        {
            return std::unexpected(written.error());
        }
        return file;
    }

    void EditorServer::HandleWritePlaySnapshot(const int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const WritePlaySnapshotCmd cmd = WritePlaySnapshotCmd::Deserialize(reader);

        const auto written = WritePlaySnapshot(cmd.scenePath);
        if (!written)
        {
            SendError(clientSocket, written.error());
            return;
        }
        Logger::Info("Wrote the play snapshot " + Utf8(*written));
        BufferWriter response;
        Protocol::WritePlaySnapshot(response, Utf8(*written));
        SendResponse(clientSocket, response.Release());
    }

    std::expected<void, std::string> EditorServer::EnterPlayMode(const std::filesystem::path &snapshotFile)
    {
        if (_playMode)
        {
            return std::unexpected("This host is playing already");
        }
        auto text = ReadTextFile(snapshotFile);
        if (!text)
        {
            return std::unexpected(text.error());
        }
        const nlohmann::json sceneJson = nlohmann::json::parse(*text, nullptr, false);
        if (sceneJson.is_discarded())
        {
            return std::unexpected("Not valid JSON: " + Utf8(snapshotFile));
        }
        std::unique_ptr<Scene> scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            return std::unexpected("Not a valid scene (see the log): " + Utf8(snapshotFile));
        }

        // Not edit mode: the components attach (OnAttach, Start) at the first game frame
        Scene *const playing = scene.get();
        SceneManager::AddScene(std::move(scene), true);
        SceneManager::ProcessAnyPendingSceneChange();
        if (SceneManager::GetCurScene() != playing)
        {
            return std::unexpected("The scene couldn't be loaded");
        }

        _playMode = true;
        _paused = false;
        _playFrame = 0;
        _openScene = playing;
        _openScenePath.clear();
        ++_sceneRevision;
        _savedRevision = _sceneRevision;
        NoteSceneChanged();
        PushSceneChanged({}, true);

        // The game's keys are what SendInput sets, never the window's; its clock starts now
        _keys.ReleaseAll();
        Input::KeySource::Set(&_keys);
        Application::GetInstance().ResetFrameClock();
        _watchAssets = false;

        Logger::Info(std::format("Playing scene '{}' from {}", playing->sceneName, Utf8(snapshotFile)));
        PushPlayState();
        return {};
    }

    void EditorServer::PushPlayState()
    {
        _events.Push("playState", nlohmann::json{{"state", _paused ? "Paused" : "Playing"}, {"frame", _playFrame}});
    }

    void EditorServer::SetPausedState(const bool paused)
    {
        if (paused == _paused)
        {
            return;
        }
        _paused = paused;
        if (!paused)
        {
            // The time spent paused isn't a frame
            Application::GetInstance().ResetFrameClock();
        }
        PushPlayState();
    }

    void EditorServer::RunGameFrame(const std::optional<double> deltaSeconds)
    {
        // The pointer and its buttons are what SendInput set: a window's own device is sampled over them otherwise
        if (Input::Mouse *mouse = Input::Mouse::Get())
        {
            uint32_t buttons = 0;
            for (int button = 0; button < Input::Mouse::ButtonCount; ++button)
            {
                if (_keys.IsMouseButtonDown(static_cast<Input::MouseButton>(button)))
                {
                    buttons |= Input::Mouse::ButtonBit(button);
                }
            }
            mouse->InjectPointer(_pointer ? *_pointer : mouse->GetPosition(), buttons);
        }

        TickOptions options;
        options.render = false; // frames are drawn when the client asks (RenderFrame)
        options.deltaSeconds = deltaSeconds;
        Application::GetInstance().Tick(options);
        ++_playFrame;
    }

    size_t EditorServer::RunPlayFrame(const std::chrono::milliseconds frameBudget)
    {
        if (!_playMode)
        {
            return ProcessCommands(frameBudget);
        }

        const auto start = std::chrono::steady_clock::now();
        if (!_paused)
        {
            try
            {
                RunGameFrame(std::nullopt);
            }
            catch (const std::exception &e)
            {
                // Another frame would only throw the same way sixty times a second
                Logger::Error(std::string("A game frame failed, so the game is paused: ") + e.what());
                SetPausedState(true);
            }
        }

        const auto deadline = start + frameBudget;
        size_t processed = 0;
        do
        {
            const auto now = std::chrono::steady_clock::now();
            // Rounded up: a fraction of a millisecond left must wait, not spin
            const auto remaining = now < deadline ? std::chrono::ceil<std::chrono::milliseconds>(deadline - now)
                                                  : std::chrono::milliseconds::zero();
            processed += ProcessCommands(remaining);
        } while (std::chrono::steady_clock::now() < deadline);
        return processed;
    }

    void EditorServer::HandleSetPaused(const int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetPausedCmd cmd = SetPausedCmd::Deserialize(reader);
        if (!_playMode)
        {
            SendError(clientSocket, NotAPlayHostError);
            return;
        }
        SetPausedState(cmd.paused);
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleStep(const int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const StepCmd cmd = StepCmd::Deserialize(reader);
        if (!_playMode)
        {
            SendError(clientSocket, NotAPlayHostError);
            return;
        }
        if (!_paused)
        {
            SendError(clientSocket, "Step runs frames of a paused game: pause it first (SetPaused)");
            return;
        }
        if (cmd.frames < 1 || cmd.frames > MaxStepFrames)
        {
            SendError(clientSocket, std::format("frames must be 1 to {}, not {}", MaxStepFrames, cmd.frames));
            return;
        }

        // Each frame is one fixed timestep of time, so it runs exactly one fixed update
        const double step = Time::GetFixedTimestep();
        for (uint32_t i = 0; i < cmd.frames && !Application::GetInstance().IsQuitRequested(); ++i)
        {
            RunGameFrame(step);
        }
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetPlayState(const int clientSocket)
    {
        BufferWriter response;
        WritePlayState(response, _playMode ? (_paused ? "Paused" : "Playing") : "Edit", _playFrame,
                       _playMode ? Time::GetTime() : 0.0f);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSendInput(const int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SendInputCmd cmd = SendInputCmd::Deserialize(reader);
        if (!_playMode)
        {
            SendError(clientSocket, NotAPlayHostError);
            return;
        }
        // All of it is checked first, so a bad event applies none of the batch
        const auto actions = ParseInputEvents(cmd.events);
        if (!actions)
        {
            SendError(clientSocket, actions.error());
            return;
        }
        for (const InputAction &action : *actions)
        {
            switch (action.kind)
            {
            case InputAction::Kind::Key:
                _keys.SetKey(action.key, action.down);
                break;
            case InputAction::Kind::MouseButton:
                _keys.SetMouseButton(action.button, action.down);
                break;
            case InputAction::Kind::Pointer:
                _pointer = Math::Vector2(action.x, action.y);
                break;
            case InputAction::Kind::Scroll:
                if (Input::Mouse *mouse = Input::Mouse::Get())
                {
                    mouse->AccumulateScroll(action.x, action.y);
                }
                break;
            case InputAction::Kind::ReleaseAll:
                _keys.ReleaseAll();
                break;
            }
        }
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEngineHealth(int clientSocket)
    {
        BufferWriter response;
        WriteEngineHealth(response, Application::GetInstance().GetHealth());
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string entityId = GetEntityTransformCmd::Deserialize(reader).entityId;

        BufferWriter response;

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        if (auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            auto entity = scene->FindGameObjectByUUID(uuid.value());
            if (entity && entity->HasPositionable())
            {
                auto &transform = entity->GetPositionable()->GetGlobalTransform();
                WriteEntityTransform(response, transform.GetPosition(), transform.GetRotation().ToEulerAngles(),
                                     transform.GetScale());
                SendResponse(clientSocket, response.Release());
                return;
            }
        }

        // An unknown entity used to get an identity transform, indistinguishable from a real one
        WriteError(response, "Entity not found (or has no transform): " + entityId);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Hierarchy and entities (#6, E4) ====================

    void EditorServer::HandleGetHierarchy(int clientSocket)
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        // Depth first, parents before children, siblings in order: the order the tree shows
        nlohmann::json nodes = nlohmann::json::array();
        size_t rootIndex = 0;
        for (const auto &root : scene->GetRootGameObjects())
        {
            if (root && !root->IsDestroyed())
            {
                AppendHierarchy(*root, std::string{}, rootIndex, nodes);
            }
            ++rootIndex;
        }

        BufferWriter response;
        WriteHierarchy(response, _sceneRevision, nodes);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateEntityEx(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const CreateEntityExCmd cmd = CreateEntityExCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        // Everything is checked before anything is made, so a refused request changes nothing
        std::shared_ptr<GameObject> parent;
        if (!cmd.parentId.empty())
        {
            parent = FindEntity(*scene, cmd.parentId);
            if (parent == nullptr)
            {
                SendError(clientSocket, NotFoundMessage("Parent", cmd.parentId));
                return;
            }
        }
        const auto &presets = EntityPresets();
        const auto preset = std::ranges::find_if(presets, [&cmd](const EntityPreset &candidate)
        {
            return candidate.name == cmd.preset;
        });
        if (preset == presets.end())
        {
            std::string known;
            for (const EntityPreset &candidate : presets)
            {
                if (!candidate.name.empty())
                {
                    known += (known.empty() ? "" : ", ") + std::string(candidate.name);
                }
            }
            SendError(clientSocket, std::format("Unknown preset '{}' (the presets are {}, or empty for an empty object)",
                                                SanitizeForLog(cmd.preset), known));
            return;
        }

        // An editor object always has a transform (CreateEntity's has none until something gives it one)
        auto gameObject = GameObject::Create(cmd.name.empty() ? std::string(preset->objectName) : cmd.name);
        gameObject->CreatePositionable();
        preset->addComponents(*gameObject);

        if (parent != nullptr)
        {
            parent->AddChild(gameObject, false);
        }
        else
        {
            scene->AddRootGameObject(gameObject);
        }
        if (cmd.siblingIndex >= 0)
        {
            gameObject->SetSiblingIndex(static_cast<size_t>(cmd.siblingIndex));
        }

        const std::string id = gameObject->GetUUID().ToString();
        MarkSceneChanged({id});
        RecordEdit(Labelled("Create", gameObject->GetName()), CreatedOp(*gameObject));
        BufferWriter response;
        WriteEntityCreated(response, id);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetEntityParent(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetEntityParentCmd cmd = SetEntityParentCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }
        std::shared_ptr<GameObject> newParent;
        if (!cmd.parentId.empty())
        {
            newParent = FindEntity(*scene, cmd.parentId);
            if (newParent == nullptr)
            {
                SendError(clientSocket, NotFoundMessage("Parent", cmd.parentId));
                return;
            }
            if (newParent == entity || newParent->IsChildOf(entity))
            {
                SendError(clientSocket, std::format("Can't make '{}' a child of itself or of its own descendant '{}'",
                                                    SanitizeForLog(entity->GetName()), SanitizeForLog(newParent->GetName())));
                return;
            }
        }

        // The place it takes: the last of its new siblings unless the request says otherwise (and a place past the
        // last is the last). Moving it within its own parent leaves one fewer sibling to count.
        const std::shared_ptr<GameObject> oldParent = entity->GetParent();
        const bool sameParent = oldParent == newParent;
        const size_t siblingCount =
            (newParent != nullptr ? newParent->GetChildCount() : scene->GetRootGameObjectCount()) + (sameParent ? 0 : 1);
        const size_t target = cmd.siblingIndex < 0 ? siblingCount - 1
                                                   : std::min(static_cast<size_t>(cmd.siblingIndex), siblingCount - 1);
        if (sameParent && target == entity->GetSiblingIndex())
        {
            // Already there: nothing changes, so the revision doesn't move (the scene isn't made unsaved)
            BufferWriter response;
            WriteOk(response);
            SendResponse(clientSocket, response.Release());
            return;
        }

        const PlacementState placedBefore = PlacementOf(*entity);
        entity->SetParent(newParent, cmd.keepWorldTransform);
        if (entity->GetParent() != newParent)
        {
            SendError(clientSocket, "The entity couldn't be moved there");
            return;
        }
        entity->SetSiblingIndex(target);

        // The subtree: its world transforms and activeInHierarchy changed with the parent
        MarkSceneChanged(SubtreeIds(*entity));
        RecordEdit(Labelled("Reparent", entity->GetName()),
                   PlacementOp(entity->GetUUID().ToString(), placedBefore, PlacementOf(*entity)));
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetEntityProperties(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetEntityPropertiesCmd cmd = SetEntityPropertiesCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }
        // All of it is checked before any of it is applied
        const std::expected<EntityProperties, std::string> parsed = ParseEntityProperties(cmd.properties);
        if (!parsed)
        {
            SendError(clientSocket, parsed.error());
            return;
        }

        bool changed = false;
        bool activeChanged = false;
        // Which properties changed, for coalescing (typing a name is one step) and for the step's label
        std::string changedNames;
        const PropertiesState propertiesBefore = PropertiesOf(*entity);
        if (parsed->name && *parsed->name != entity->GetName())
        {
            entity->SetName(*parsed->name);
            changed = true;
            changedNames += "name,";
        }
        if (parsed->tag && *parsed->tag != entity->GetTag())
        {
            entity->SetTag(*parsed->tag);
            changed = true;
            changedNames += "tag,";
        }
        if (parsed->layer && *parsed->layer != entity->GetLayer())
        {
            entity->SetLayer(*parsed->layer);
            changed = true;
            changedNames += "layer,";
        }
        if (parsed->active && *parsed->active != entity->IsActive())
        {
            entity->SetActive(*parsed->active);
            changed = true;
            activeChanged = true;
            changedNames += "active,";
        }
        if (changed)
        {
            // The active state reaches the whole subtree (activeInHierarchy), so a client refetches it all
            MarkSceneChanged(activeChanged ? SubtreeIds(*entity) : std::vector<std::string>{entity->GetUUID().ToString()});
            RecordEdit(changedNames == "name," ? Labelled("Rename", propertiesBefore.name) : Labelled("Edit", entity->GetName()),
                       PropertiesOp(entity->GetUUID().ToString(), propertiesBefore, PropertiesOf(*entity), changedNames));
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleDuplicateEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const DuplicateEntityCmd cmd = DuplicateEntityCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> source = FindEntity(*scene, cmd.entityId);
        if (source == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        // The subtree as it is saved, built again with fresh UUIDs for every object and component. References
        // between its own objects point at the copies; references to anything else in the scene are kept (the
        // scene's own references are the fallback), so a copied script still points at the same target.
        const ReferenceResolver sceneReferences = SceneReferences(*scene);
        const std::shared_ptr<GameObject> copy = PrefabManager::InstantiatePrefab(source->Serialize(), sceneReferences);
        if (copy == nullptr)
        {
            SendError(clientSocket, "The entity couldn't be duplicated (see the log)");
            return;
        }

        // Next to the original, under its parent
        const std::shared_ptr<GameObject> parent = source->GetParent();
        copy->SetName(DuplicateName(*source, parent != nullptr ? parent->GetChildren() : scene->GetRootGameObjects()));
        if (parent != nullptr)
        {
            parent->AddChild(copy, false);
        }
        else
        {
            scene->AddRootGameObject(copy);
        }
        copy->SetSiblingIndex(source->GetSiblingIndex() + 1);

        MarkSceneChanged(SubtreeIds(*copy));
        RecordEdit(Labelled("Duplicate", source->GetName()), CreatedOp(*copy));
        BufferWriter response;
        WriteEntityCreated(response, copy->GetUUID().ToString());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetEntityCmd cmd = GetEntityCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        // An object without a transform is at the origin of the world
        const Positionable *positionable = entity->GetPositionable();
        const Math::Matrix<float, 4, 4> worldMatrix =
            positionable != nullptr ? positionable->GetLocalToWorldMatrix() : Math::Matrix<float, 4, 4>::identity();

        BufferWriter response;
        WriteEntityData(response, EntityDetailsJson(*entity), worldMatrix);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetLocalTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetLocalTransformCmd cmd = SetLocalTransformCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        const auto finite = [](const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const Math::Quaternion &rotation = cmd.rotation;
        const bool rotationFinite = std::isfinite(rotation.GetX()) && std::isfinite(rotation.GetY()) &&
                                    std::isfinite(rotation.GetZ()) && std::isfinite(rotation.GetW());
        if (!finite(cmd.position) || !finite(cmd.scale) || !rotationFinite)
        {
            SendError(clientSocket, "The transform has a value that isn't a finite number");
            return;
        }
        // A client sends a rotation it may have accumulated rounding in: it is normalised, and only a quaternion
        // with no direction at all (all zeros) is refused
        const float rotationLength = rotation.Length();
        if (!std::isfinite(rotationLength) || !(rotationLength > 1e-6f))
        {
            SendError(clientSocket, "The rotation is a zero quaternion, or too large to normalise");
            return;
        }

        // The transform as it will be stored: already that, and nothing changes (the revision doesn't move)
        const Math::Quaternion normalised = rotation.Normalized();
        const Positionable *existing = entity->GetPositionable();
        if (existing != nullptr && existing->GetLocalPosition() == cmd.position &&
            existing->GetLocalRotation() == normalised && existing->GetLocalScale() == cmd.scale)
        {
            BufferWriter unchanged;
            WriteOk(unchanged);
            SendResponse(clientSocket, unchanged.Release());
            return;
        }

        // An object CreateEntity made has no transform: setting one gives it one
        const LocalTransformState transformBefore = LocalTransformOf(*entity);
        entity->CreatePositionable();
        Positionable *positionable = entity->GetPositionable();
        positionable->SetLocalPositionAndRotation(cmd.position, normalised);
        positionable->SetLocalScale(cmd.scale);

        // The subtree's world matrices moved with it
        MarkSceneChanged(SubtreeIds(*entity));
        RecordEdit(Labelled("Transform", entity->GetName()),
                   TransformOp(entity->GetUUID().ToString(), transformBefore, LocalTransformOf(*entity)));
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Components (#6, E5) ====================

    void EditorServer::HandleGetComponentTypes(int clientSocket)
    {
        ComponentRegistry &registry = ComponentRegistry::Instance();
        std::vector<std::string> names = registry.GetRegisteredTypes();
        std::ranges::sort(names);

        nlohmann::json types = nlohmann::json::array();
        for (const std::string &name : names)
        {
            if (const std::optional<ComponentSchema> schema = registry.Describe(name))
            {
                types.push_back(schema->ToJson());
            }
        }

        BufferWriter response;
        WriteComponentTypes(response, types);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleAddComponent(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const AddComponentCmd cmd = AddComponentCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        ComponentRegistry &registry = ComponentRegistry::Instance();
        if (!registry.IsRegistered(cmd.typeName))
        {
            SendError(clientSocket, std::format("Unknown component type '{}'", SanitizeForLog(cmd.typeName)));
            return;
        }
        if (registry.IsSingleton(cmd.typeName))
        {
            for (const auto &existing : entity->GetAllComponents())
            {
                if (existing->GetTypeName() == cmd.typeName)
                {
                    SendError(clientSocket, std::format("'{}' can only be added once to an object", SanitizeForLog(cmd.typeName)));
                    return;
                }
            }
        }

        Component *added = entity->AddComponent(registry.Create(cmd.typeName, *entity));
        if (added == nullptr)
        {
            SendError(clientSocket, std::format("A '{}' couldn't be made", SanitizeForLog(cmd.typeName)));
            return;
        }

        MarkSceneChanged({entity->GetUUID().ToString()});
        RecordEdit(Labelled("Add", cmd.typeName),
                   AddedComponentOp(entity->GetUUID().ToString(), added->GetUUID().ToString(), cmd.typeName,
                                    added->Serialize()));
        BufferWriter response;
        WriteComponentAdded(response, added->GetUUID().ToString(), added->Serialize());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleRemoveComponent(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const RemoveComponentCmd cmd = RemoveComponentCmd::Deserialize(reader);

        const auto target = FindComponentTarget(cmd.entityId, cmd.componentId);
        if (!target)
        {
            SendError(clientSocket, target.error());
            return;
        }
        // What undoing needs: the scene as it is, since removing clears every reference to the component elsewhere,
        // which putting the component back alone wouldn't restore (a load resolves them again, by UUID)
        std::shared_ptr<const std::string> snapshot;
        if (SceneManager::GetCurScene()->IsEditMode())
        {
            snapshot = std::make_shared<const std::string>(SnapshotScene(*SceneManager::GetCurScene()));
            if (_history.InGroup() && _history.GroupBytes() > 0 && _history.GroupBytes() + snapshot->size() > _history.GetMaxBytes())
            {
                SendError(clientSocket, "The open edit group already holds too much to undo: end it first");
                return;
            }
        }
        const std::string entityId = target->entity->GetUUID().ToString();
        const std::string componentId = target->component->GetUUID().ToString();
        const std::string label = Labelled("Remove", target->component->GetTypeName());
        // The objects whose references to the component the removal clears are changed too: found before it does
        std::vector<std::string> changedIds{entityId};
        AppendNewIds(changedIds, HoldersOfComponent(*SceneManager::GetCurScene(), *target->component));
        if (!target->entity->RemoveComponent(target->component))
        {
            SendError(clientSocket, "The component couldn't be removed");
            return;
        }

        MarkSceneChanged(std::move(changedIds));
        if (snapshot != nullptr)
        {
            EditOp op;
            op.undo = [this, snapshot]() -> EditOutcome { return RestoreSceneSnapshot(*snapshot); };
            op.redo = [entityId, componentId]() -> EditOutcome { return RemoveComponentById(entityId, componentId); };
            op.undoBytes = snapshot->size();
            RecordEdit(label, std::move(op));
        }
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetComponentFields(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetComponentFieldsCmd cmd = SetComponentFieldsCmd::Deserialize(reader);

        const auto target = FindComponentTarget(cmd.entityId, cmd.componentId);
        if (!target)
        {
            SendError(clientSocket, target.error());
            return;
        }

        std::vector<std::string> changed;
        const nlohmann::json before = target->component->Serialize();
        const std::expected<nlohmann::json, std::string> stored =
            SetComponentValues(*SceneManager::GetCurScene(), *target->component, cmd.values, changed);
        if (!stored)
        {
            SendError(clientSocket, stored.error());
            return;
        }

        if (!changed.empty())
        {
            target->component->OnEditorFieldsChanged(changed);
            MarkSceneChanged({target->entity->GetUUID().ToString()});

            // The fields the request named, so typing in one field coalesces and a paste of several is its own step. A
            // script's fields are inside the container (scriptData): they are named one by one, "scriptData.speed", so
            // two fields of one script aren't merged into each other, and the label says which.
            const std::vector<FieldInfo> fields = target->component->DescribeFields();
            std::vector<std::string> requested;
            std::vector<std::string> leaves;
            if (cmd.values.is_object())
            {
                for (const auto &[key, value] : cmd.values.items())
                {
                    const std::string topKey = key;
                    requested.push_back(topKey);
                    const bool isContainer = value.is_object() && std::ranges::any_of(fields, [&topKey](const FieldInfo &field)
                    {
                        return field.container == topKey;
                    });
                    if (isContainer)
                    {
                        for (const auto &[inner, innerValue] : value.items())
                        {
                            leaves.push_back(topKey + "." + inner);
                        }
                    }
                    else
                    {
                        leaves.push_back(topKey);
                    }
                }
            }
            std::ranges::sort(requested);
            std::ranges::sort(leaves);
            std::string requestedKeys;
            for (const std::string &leaf : leaves)
            {
                requestedKeys += leaf + ",";
            }
            std::string label = Labelled("Edit", target->component->GetTypeName());
            if (leaves.size() == 1)
            {
                label = Labelled("Set", std::string_view(leaves.front()).substr(leaves.front().rfind('.') + 1));
            }
            RecordEdit(std::move(label),
                       ComponentFieldsOp(target->entity->GetUUID().ToString(), target->component->GetUUID().ToString(),
                                         before, stored.value(), requested, requestedKeys));
        }
        BufferWriter response;
        WriteComponentData(response, stored.value());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetComponent(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetComponentCmd cmd = GetComponentCmd::Deserialize(reader);

        const auto target = FindComponentTarget(cmd.entityId, cmd.componentId);
        if (!target)
        {
            SendError(clientSocket, target.error());
            return;
        }

        BufferWriter response;
        WriteComponentData(response, target->component->Serialize());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetLuaFields(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetLuaFieldsCmd cmd = GetLuaFieldsCmd::Deserialize(reader);

        const auto target = FindComponentTarget(cmd.entityId, cmd.componentId);
        if (!target)
        {
            SendError(clientSocket, target.error());
            return;
        }
        if (target->component->GetTypeName() != "LuaComponent")
        {
            SendError(clientSocket, std::format("Component {} is a {}, not a LuaComponent",
                                                SanitizeForLog(cmd.componentId), SanitizeForLog(target->component->GetTypeName())));
            return;
        }

        ComponentSchema schema;
        schema.typeName = target->component->GetTypeName();
        schema.fields = target->component->DescribeFields();
        schema.singleton = ComponentRegistry::Instance().IsSingleton(schema.typeName);

        BufferWriter response;
        WriteLuaFields(response, schema.ToJson());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateScriptCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            std::string scriptTemplate = Scripting::MakeLuaScriptTemplate(cmd.name);
            WriteScriptData(response, scriptTemplate);
            Logger::Info("Generated script template for: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to create script: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleRescanAssets(int clientSocket)
    {
        ApplyAssetChanges(IO::ResourceLoader::Instance().RescanAssets());

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    // ==================== Assets and the file watcher (#81, E8) ====================

    bool EditorServer::IsValidAssetName(const std::string_view name)
    {
        if (name.empty() || name.size() > 255 || name == "." || name == "..")
        {
            return false;
        }
        for (const char c : name)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F || std::string_view("<>:\"|?*\\/").find(c) != std::string_view::npos)
            {
                return false;
            }
        }
        if (name.back() == '.' || name.back() == ' ')
        {
            return false;
        }
        // A Windows device name, whatever follows its dot ("nul.txt" is the device too)
        std::string stem = LowerAscii(name.substr(0, name.find('.')));
        while (!stem.empty() && stem.back() == ' ')
        {
            stem.pop_back();
        }
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        {
            return false;
        }
        return !((stem.starts_with("com") || stem.starts_with("lpt")) && stem.size() == 4 && stem[3] >= '1' &&
                 stem[3] <= '9');
    }

    bool EditorServer::IsTextAssetPath(const std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        const std::size_t slash = path.find_last_of('/');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        {
            return false;
        }
        const std::string extension = LowerAscii(path.substr(dot));
        return std::ranges::find(TextAssetExtensions, std::string_view(extension)) != TextAssetExtensions.end();
    }

    bool EditorServer::IsValidUtf8(const std::string_view text)
    {
        std::size_t i = 0;
        const std::size_t size = text.size();
        while (i < size)
        {
            const auto lead = static_cast<unsigned char>(text[i]);
            if (lead < 0x80)
            {
                ++i;
                continue;
            }
            std::size_t extra = 0;
            std::uint32_t codePoint = 0;
            std::uint32_t minimum = 0;
            if ((lead & 0xE0) == 0xC0)
            {
                extra = 1;
                codePoint = lead & 0x1Fu;
                minimum = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                extra = 2;
                codePoint = lead & 0x0Fu;
                minimum = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                extra = 3;
                codePoint = lead & 0x07u;
                minimum = 0x10000;
            }
            else
            {
                return false;
            }
            if (size - i <= extra)
            {
                return false;
            }
            for (std::size_t k = 1; k <= extra; ++k)
            {
                const auto next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0) != 0x80)
                {
                    return false;
                }
                codePoint = (codePoint << 6) | (next & 0x3Fu);
            }
            if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    std::expected<ResolvedScenePath, std::string> EditorServer::ResolveAssetPath(
        const std::filesystem::path &assetsRoot, const std::string &path, const bool allowRoot)
    {
        const std::string shown = SanitizeForLog(path);
        if (!path.starts_with("res://") || path.find('\0') != std::string::npos)
        {
            return std::unexpected(std::format("Not an asset path: '{}' (expected res://<folder>/<name>)", shown));
        }
        const IO::ResourcePath resourcePath(path);
        const std::string &relativeText = resourcePath.GetPath();

        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(assetsRoot, error);
        if (error)
        {
            return std::unexpected("The project's assets folder can't be resolved");
        }
        if (relativeText.empty())
        {
            if (!allowRoot)
            {
                return std::unexpected(std::format("Not a path inside the assets folder: '{}'", shown));
            }
            return ResolvedScenePath{IO::ResourcePath(IO::PathType::Resource, ""), root};
        }

        // Every part is a name a file can have: this rules out "..", a drive ("C:"), an NTFS stream ("name:stream")
        // and the characters and device names Windows refuses
        for (std::size_t start = 0; start <= relativeText.size();)
        {
            std::size_t end = relativeText.find('/', start);
            if (end == std::string::npos)
            {
                end = relativeText.size();
            }
            const std::string part = relativeText.substr(start, end - start);
            if (!IsValidAssetName(part))
            {
                return std::unexpected(std::format("Not a valid asset path: '{}' ('{}' isn't a name a file can have)",
                                                   shown, SanitizeForLog(part)));
            }
            start = end + 1;
        }

        // Lexically inside the assets folder, then also with symlinks resolved for the part that exists (as
        // ResolveScenePath does), so a link inside assets/ can't lead a read or write elsewhere
        const std::filesystem::path relative = FromUtf8(relativeText);
        const std::filesystem::path file = (root / relative).lexically_normal();
        const std::filesystem::path parent = std::filesystem::weakly_canonical(file.parent_path(), error);
        if (error)
        {
            return std::unexpected(std::format("Can't resolve the folder of '{}'", shown));
        }
        const auto outside = [](const std::filesystem::path &fromRoot)
        {
            return fromRoot.empty() || *fromRoot.begin() == "..";
        };
        const std::filesystem::path lexical = file.lexically_relative(root);
        const std::filesystem::path resolvedParent = parent.lexically_relative(root);
        if (outside(lexical) || (resolvedParent != "." && outside(resolvedParent)))
        {
            return std::unexpected(std::format("Not a path inside the project's assets folder: '{}'", shown));
        }

        // An existing file or folder is resolved itself too (a symlink in the way can't lead out of the folder), and
        // the path takes the spelling the file system has
        std::filesystem::path target = parent / file.filename();
        if (std::error_code existsError; std::filesystem::exists(target, existsError))
        {
            target = std::filesystem::weakly_canonical(target, error);
            if (error)
            {
                return std::unexpected(std::format("Can't resolve '{}'", shown));
            }
        }
        const std::filesystem::path fromRoot = target.lexically_relative(root);
        if (outside(fromRoot))
        {
            return std::unexpected(std::format("Not a path inside the project's assets folder: '{}'", shown));
        }
        return ResolvedScenePath{IO::ResourcePath(IO::PathType::Resource, IO::PathToUtf8(fromRoot)), std::move(target)};
    }

    void EditorServer::ReloadLoadedAsset(const IO::ResourcePath &path)
    {
        auto &loader = IO::ResourceLoader::Instance();
        try
        {
            const std::shared_ptr<Base::Asset> held = loader.GetCached<Base::Asset>(path);
            if (!held)
            {
                return; // nothing was made from the old file: the next load reads the new one
            }
            if (const auto script = std::dynamic_pointer_cast<LuaScript>(held))
            {
                // The same object stays in the cache: LuaComponents point at it, and ReloadModule makes them rebuild
                // their instances from its source. An error in the new source is logged by the runtime.
                auto text = ReadTextFile(loader.Resolve(path));
                if (!text)
                {
                    Logger::Warn("Can't reload " + path.ToString() + ": " + text.error());
                    return;
                }
                script->SetSourceCode(*text);
                Scripting::LuaRuntime::Instance().ReloadModule(path, script.get());
                return;
            }
            (void)loader.Reload(path);
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::format("Can't reload {}: {}", path.ToString(), SanitizeForLog(e.what(), 300)));
        }
        catch (...)
        {
            Logger::Error("Can't reload " + path.ToString());
        }
    }

    void EditorServer::ApplyAssetChanges(const IO::ResourceLoader::RescanResult &changes)
    {
        if (changes.Empty())
        {
            return;
        }
        for (const IO::ResourcePath &path : changes.modified)
        {
            ReloadLoadedAsset(path);
        }
        // An asset the scene draws (a texture, a mesh) may have changed
        NoteViewChanged();
        _events.Push("assetsChanged", nlohmann::json{{"added", PathStrings(changes.added)},
                                                     {"removed", PathStrings(changes.removed)},
                                                     {"modified", PathStrings(changes.modified)}});
    }

    void EditorServer::NoteAssetWritten(const IO::ResourcePath &path, const bool force)
    {
        IO::ResourceLoader::RescanResult changes;
        switch (IO::ResourceLoader::Instance().RefreshAsset(path))
        {
        case IO::ResourceLoader::RefreshResult::Added:
            changes.added.push_back(path);
            break;
        case IO::ResourceLoader::RefreshResult::Modified:
            changes.modified.push_back(path);
            break;
        case IO::ResourceLoader::RefreshResult::Unchanged:
            if (force)
            {
                changes.modified.push_back(path);
            }
            break;
        case IO::ResourceLoader::RefreshResult::Missing:
            break; // no loader for its extension: not an asset
        }
        ApplyAssetChanges(changes);
    }

    bool EditorServer::PollAssets()
    {
        if (!_project)
        {
            return false;
        }
        auto &loader = IO::ResourceLoader::Instance();
        try
        {
            if (!loader.AssetsChangedOnDisk())
            {
                return false;
            }
            const IO::ResourceLoader::RescanResult changes = loader.RescanAssets();
            ApplyAssetChanges(changes);
            _assetPollFailureLogged = false;
            return !changes.Empty();
        }
        catch (const std::exception &e)
        {
            // The folder changed under the scan (a delete, a rename): the next check tries again. Said once, so a
            // folder that stays unreadable doesn't log every second.
            if (!_assetPollFailureLogged)
            {
                _assetPollFailureLogged = true;
                Logger::Warn(std::format("The asset watcher couldn't scan the assets folder: {}",
                                         SanitizeForLog(e.what(), 300)));
            }
            return false;
        }
    }

    bool EditorServer::PollAssetsIfDue()
    {
        // A play host doesn't watch: the host that edits owns the asset files and the state kept about them
        if (!_watchAssets || !_project || !_clientConnected || _playMode)
        {
            return false;
        }
        const auto now = _assetPollClock ? _assetPollClock() : std::chrono::steady_clock::now();
        if (_lastAssetPoll && now - *_lastAssetPoll < _assetPollInterval)
        {
            return false;
        }
        _lastAssetPoll = now;
        return PollAssets();
    }

    void EditorServer::HandleListAssets(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const ListAssetsCmd cmd = ListAssetsCmd::Deserialize(reader);
        // Never logs (a polled-rate command): a refusal is the Error response
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        const auto resolved =
            ResolveAssetPath(_project->root / "assets", cmd.folder.empty() ? std::string("res://") : cmd.folder, true);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(resolved->file, error))
        {
            SendError(clientSocket, "Not a folder: " + SanitizeForLog(cmd.folder));
            return;
        }
        // "" for the assets folder itself, else the folder's path under it, as the file system spells it
        const std::string prefix = resolved->resourcePath.GetPath();

        // Folders come from the file system (the metadata doesn't know empty ones); links are left out, so a link
        // can't lead the walk out of the project
        std::vector<std::string> folderPaths;
        std::filesystem::recursive_directory_iterator it(resolved->file,
                                                         std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && it != end; it.increment(error))
        {
            const std::filesystem::directory_entry &entry = *it;
            std::error_code entryError;
            if (entry.is_symlink(entryError))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_directory(entryError))
            {
                continue;
            }
            if (!cmd.recursive)
            {
                it.disable_recursion_pending();
            }
            const std::string relative = IO::PathToUtf8(entry.path().lexically_relative(resolved->file));
            folderPaths.push_back("res://" + (prefix.empty() ? relative : prefix + "/" + relative));
        }
        std::sort(folderPaths.begin(), folderPaths.end());

        std::vector<IO::AssetMetadata> metas = IO::ResourceLoader::Instance().GetAllAssets();
        std::erase_if(metas, [&](const IO::AssetMetadata &meta)
        {
            if (meta.resourcePath.GetType() != IO::PathType::Resource)
            {
                return true;
            }
            const std::string &assetPath = meta.resourcePath.GetPath();
            std::size_t nameStart = 0;
            if (!prefix.empty())
            {
                if (assetPath.size() <= prefix.size() || !assetPath.starts_with(prefix) ||
                    assetPath[prefix.size()] != '/')
                {
                    return true;
                }
                nameStart = prefix.size() + 1;
            }
            // Its own file, or (recursive) anywhere below
            return !cmd.recursive && assetPath.find('/', nameStart) != std::string::npos;
        });
        std::sort(metas.begin(), metas.end(), [](const IO::AssetMetadata &a, const IO::AssetMetadata &b)
        {
            return a.resourcePath.GetPath() < b.resourcePath.GetPath();
        });

        nlohmann::json folders = nlohmann::json::array();
        for (std::string &folder : folderPaths)
        {
            folders.push_back(std::move(folder));
        }
        nlohmann::json assets = nlohmann::json::array();
        for (const IO::AssetMetadata &meta : metas)
        {
            assets.push_back(AssetInfoJson(meta));
        }

        BufferWriter response;
        WriteAssetList(response, folders, assets);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetAssetInfo(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetAssetInfoCmd cmd = GetAssetInfoCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        auto &loader = IO::ResourceLoader::Instance();

        const IO::AssetMetadata *meta = nullptr;
        if (cmd.uuidOrPath.starts_with("res://"))
        {
            const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.uuidOrPath);
            if (!resolved)
            {
                SendError(clientSocket, resolved.error());
                return;
            }
            meta = loader.GetMetadata(resolved->resourcePath);
            if (meta == nullptr)
            {
                SendError(clientSocket, "Not an asset: " + SanitizeForLog(resolved->resourcePath.ToString()));
                return;
            }
        }
        else if (const auto uuid = Math::UUID::FromString(cmd.uuidOrPath); uuid.has_value())
        {
            meta = loader.GetMetadata(uuid.value());
            if (meta == nullptr)
            {
                if (const auto *location = loader.FindSubAssetLocation(uuid.value()))
                {
                    SendError(clientSocket, std::format("{} is a part of {} ({}): ask for the file",
                                                        uuid.value().ToString(), location->parent.ToString(),
                                                        SanitizeForLog(location->key)));
                }
                else
                {
                    SendError(clientSocket, "No asset has the UUID " + uuid.value().ToString());
                }
                return;
            }
        }
        else
        {
            SendError(clientSocket, "Expected a res:// path or an asset UUID: " + SanitizeForLog(cmd.uuidOrPath));
            return;
        }

        nlohmann::json info = AssetInfoJson(*meta);
        // The import settings, without the sub-asset index (it is listed as subAssets)
        nlohmann::json customData = meta->customData.is_object() ? meta->customData : nlohmann::json::object();
        customData.erase("subAssets");
        customData.erase("subAssetsSource");
        info["customData"] = std::move(customData);
        info["loaded"] = loader.GetCached<Base::Asset>(meta->resourcePath) != nullptr;

        BufferWriter response;
        WriteAssetDetail(response, info);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetImportSettings(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetImportSettingsCmd cmd = SetImportSettingsCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        if (!cmd.customData.is_object())
        {
            SendError(clientSocket, "customData must be a JSON object");
            return;
        }
        const std::size_t size = cmd.customData.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size();
        if (size > MaxImportSettingsBytes)
        {
            SendError(clientSocket, std::format("customData is {} bytes of JSON; at most {} are accepted", size,
                                                MaxImportSettingsBytes));
            return;
        }
        const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.path);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        auto &loader = IO::ResourceLoader::Instance();
        if (!loader.Exists(resolved->resourcePath))
        {
            SendError(clientSocket, "Not an asset: " + SanitizeForLog(resolved->resourcePath.ToString()));
            return;
        }
        if (auto saved = loader.SetImportSettings(resolved->resourcePath, cmd.customData); !saved)
        {
            SendError(clientSocket, "Couldn't change the import settings: " + saved.error());
            return;
        }

        // An asset made with the old settings is dropped, so the next use loads it with the new ones. A script has no
        // import settings, and LuaComponents point at its object, so it is never dropped.
        const std::shared_ptr<Base::Asset> held = loader.GetCached<Base::Asset>(resolved->resourcePath);
        if (held && std::dynamic_pointer_cast<LuaScript>(held) == nullptr)
        {
            (void)loader.Reload(resolved->resourcePath);
        }
        NoteViewChanged();
        _events.Push("assetsChanged", nlohmann::json{{"added", nlohmann::json::array()},
                                                     {"removed", nlohmann::json::array()},
                                                     {"modified", nlohmann::json::array({resolved->resourcePath.ToString()})}});
        Logger::Info("Changed the import settings of " + resolved->resourcePath.ToString());

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleReadTextAsset(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const ReadTextAssetCmd cmd = ReadTextAssetCmd::Deserialize(reader);
        // Never logs (a polled-rate command): a refusal is the Error response
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.path);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        const std::string shown = SanitizeForLog(resolved->resourcePath.ToString());
        if (!IsTextAssetPath(resolved->resourcePath.GetPath()))
        {
            SendError(clientSocket, "Not a text asset: " + shown + " (its extension isn't one ReadTextAsset reads)");
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(resolved->file, error))
        {
            SendError(clientSocket, "File not found: " + shown);
            return;
        }
        const std::uintmax_t fileSize = std::filesystem::file_size(resolved->file, error);
        if (error || fileSize > MaxTextAssetBytes)
        {
            SendError(clientSocket, std::format("{} is too large to read as text (at most {} bytes)", shown,
                                                MaxTextAssetBytes));
            return;
        }
        auto text = ReadTextFile(resolved->file);
        if (!text)
        {
            SendError(clientSocket, "Couldn't read " + shown + ": " + text.error());
            return;
        }
        if (!IsValidUtf8(*text))
        {
            SendError(clientSocket, shown + " isn't valid UTF-8 text");
            return;
        }

        BufferWriter response;
        WriteTextData(response, *text);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleWriteTextAsset(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const WriteTextAssetCmd cmd = WriteTextAssetCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.path);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        const std::string shown = SanitizeForLog(resolved->resourcePath.ToString());
        if (!IsTextAssetPath(resolved->resourcePath.GetPath()))
        {
            SendError(clientSocket, "Not a text asset: " + shown + " (its extension isn't one WriteTextAsset writes)");
            return;
        }
        if (cmd.text.size() > MaxTextAssetBytes)
        {
            SendError(clientSocket, std::format("The text is {} bytes; at most {} are accepted", cmd.text.size(),
                                                MaxTextAssetBytes));
            return;
        }
        if (cmd.text.find('\0') != std::string::npos || !IsValidUtf8(cmd.text))
        {
            SendError(clientSocket, "The text must be valid UTF-8 without NUL characters");
            return;
        }
        if (resolved->resourcePath.ToString() == OpenScenePath())
        {
            SendError(clientSocket, shown + " is the open scene's file: save the scene with SaveSceneToFile");
            return;
        }
        std::error_code error;
        if (std::filesystem::exists(resolved->file, error) && !std::filesystem::is_regular_file(resolved->file, error))
        {
            SendError(clientSocket, shown + " isn't a file");
            return;
        }
        if (!std::filesystem::is_directory(resolved->file.parent_path(), error))
        {
            SendError(clientSocket, "The folder of " + shown + " doesn't exist (create it with CreateFolder)");
            return;
        }
        if (auto written = IO::WriteTextFileAtomically(resolved->file, cmd.text); !written)
        {
            SendError(clientSocket, "Couldn't write " + shown + ": " + written.error());
            return;
        }
        NoteAssetWritten(resolved->resourcePath, true);
        Logger::Info("Wrote " + shown);

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateScriptAsset(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const CreateScriptAssetCmd cmd = CreateScriptAssetCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.path);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        const std::string shown = SanitizeForLog(resolved->resourcePath.ToString());
        if (LowerAscii(resolved->resourcePath.GetExtension()) != ".lua" || resolved->resourcePath.GetStem().empty())
        {
            SendError(clientSocket, "A script's path must end in .lua: " + shown);
            return;
        }
        std::error_code error;
        if (std::filesystem::exists(resolved->file, error))
        {
            SendError(clientSocket, shown + " already exists");
            return;
        }

        const std::string className = cmd.className.empty() ? resolved->resourcePath.GetStem() : cmd.className;
        const std::string scriptText = Scripting::MakeLuaScriptTemplate(className);
        std::filesystem::create_directories(resolved->file.parent_path(), error);
        if (error)
        {
            SendError(clientSocket, "Couldn't create the folder of " + shown + ": " + error.message());
            return;
        }
        if (auto written = IO::WriteTextFileAtomically(resolved->file, scriptText); !written)
        {
            SendError(clientSocket, "Couldn't write " + shown + ": " + written.error());
            return;
        }
        NoteAssetWritten(resolved->resourcePath, false);
        Logger::Info("Created script " + shown);

        const Math::UUID uuid = IO::ResourceLoader::Instance().GetUUID(resolved->resourcePath);
        BufferWriter response;
        WriteAssetCreated(response, resolved->resourcePath.ToString(),
                          (uuid == Math::UUID::ZERO ? IO::ResourceUUID::FromPath(resolved->resourcePath) : uuid).ToString());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateFolder(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const CreateFolderCmd cmd = CreateFolderCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        const auto resolved = ResolveAssetPath(_project->root / "assets", cmd.path);
        if (!resolved)
        {
            SendError(clientSocket, resolved.error());
            return;
        }
        const std::string shown = SanitizeForLog(resolved->resourcePath.ToString());
        std::error_code error;
        if (std::filesystem::exists(resolved->file, error))
        {
            if (!std::filesystem::is_directory(resolved->file, error))
            {
                SendError(clientSocket, "A file is in the way: " + shown);
                return;
            }
        }
        else
        {
            std::filesystem::create_directories(resolved->file, error);
            if (error)
            {
                SendError(clientSocket, "Couldn't create " + shown + ": " + error.message());
                return;
            }
            Logger::Info("Created folder " + shown);
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Undo, redo and autosave (#6, E6) ====================

    std::string EditorServer::SnapshotScene(const Scene &scene)
    {
        return scene.Serialize().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    EditOutcome EditorServer::RestoreSceneSnapshot(const std::string &snapshot)
    {
        Scene *const current = SceneManager::GetCurScene();
        if (current == nullptr)
        {
            return std::unexpected(std::string("No scene loaded"));
        }
        const nlohmann::json sceneJson = nlohmann::json::parse(snapshot, nullptr, false);
        if (sceneJson.is_discarded())
        {
            return std::unexpected(std::string("The scene snapshot isn't valid JSON"));
        }
        // Built before anything is touched, so a snapshot that can't be built leaves the scene as it is
        std::unique_ptr<Scene> scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            return std::unexpected(std::string("The scene couldn't be built again from its snapshot (see the log)"));
        }

        // The same scene as far as a client can tell: its UUID, its file, edit mode
        scene->sceneName = current->sceneName;
        scene->SetUUID(current->GetUUID());
        scene->SetResourcePath(current->GetResourcePath());
        scene->SetEditMode(true);
        Scene *const restored = scene.get();
        const bool wasOpenScene = current == _openScene;

        SceneManager::AddScene(std::move(scene), true);
        SceneManager::ProcessAnyPendingSceneChange();
        if (SceneManager::GetCurScene() != restored)
        {
            return std::unexpected(std::string("The scene couldn't be loaded"));
        }
        if (wasOpenScene)
        {
            _openScene = restored;
        }

        // Every object is new: the ids are the same, but a client refetches everything
        EditEffect effect;
        effect.full = true;
        return effect;
    }

    void EditorServer::PushHistoryChanged()
    {
        _events.Push("historyChanged", nlohmann::json{{"canUndo", _history.CanUndo()},
                                                       {"canRedo", _history.CanRedo()},
                                                       {"label", _history.UndoLabel()},
                                                       {"redoLabel", _history.RedoLabel()},
                                                       {"undoCount", _history.Cursor()},
                                                       {"redoCount", _history.StepCount() - _history.Cursor()}});
    }

    void EditorServer::RecordEdit(std::string label, EditOp op)
    {
        // Undo is for a scene opened for editing: in any other (a host that plays) the ops would run component
        // callbacks and rebuild the scene as an edit-mode one
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr || !scene->IsEditMode())
        {
            return;
        }
        const EditHistory::Recorded recorded = _history.Record(std::move(label), std::move(op));
        if (recorded == EditHistory::Recorded::NewStep)
        {
            PushHistoryChanged();
            NoteEditStepEnded();
        }
        else if (recorded == EditHistory::Recorded::Coalesced)
        {
            // The step before it changed (its label and place did not): no new event, but the scene did change
            NoteEditStepEnded();
        }
    }

    void EditorServer::CloseEditGroups()
    {
        const EditHistory::GroupEnd ended = _history.CloseGroups();
        if (ended != EditHistory::GroupEnd::NotOpen)
        {
            // CanUndo and CanRedo are false while a group is open, and may not be now
            PushHistoryChanged();
        }
        if (ended == EditHistory::GroupEnd::Committed)
        {
            NoteEditStepEnded();
            FlushAutosave();
        }
        else if (ended == EditHistory::GroupEnd::NoChange)
        {
            NoteGroupWithoutChange();
            FlushAutosave();
        }
    }

    void EditorServer::NoteGroupWithoutChange()
    {
        // The group's writes each moved the scene revision, and the scene is what it was before them: it is as saved as
        // it was (a state that was saved stays so, one that was not stays unsaved), and an autosave written for the
        // writes is out of date. The event carries the saved revision.
        if (_history.IsAtSavedState())
        {
            _savedRevision = _sceneRevision;
        }
        NoteEditStepEnded();
        PushSceneChanged();
    }

    void EditorServer::UndoOrRedo(int clientSocket, const bool undo)
    {
        if (SceneManager::GetCurScene() == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        if (!SceneManager::GetCurScene()->IsEditMode())
        {
            SendError(clientSocket, "Undo and redo work on a scene opened for editing, not on one that runs");
            return;
        }

        const size_t stepsBefore = _history.StepCount();
        std::expected<EditHistory::Applied, std::string> applied = undo ? _history.Undo() : _history.Redo();
        if (!applied)
        {
            if (_history.StepCount() != stepsBefore)
            {
                // A step failed halfway and the history was cleared: the scene may have changed in a way no step
                // describes, so a client refetches everything
                MarkSceneChanged({}, true);
                PushHistoryChanged();
            }
            SendError(clientSocket, applied.error());
            return;
        }

        // A change like any other (the revision only grows), and the scene is saved again when the step led back to
        // the state that was saved
        ++_sceneRevision;
        NoteSceneChanged();
        if (applied->effect.inexact)
        {
            // The scene isn't exactly what was saved even if the history says it is (a transform the object didn't
            // have before can't be taken away again)
            _history.ForgetSaved();
        }
        if (_history.IsAtSavedState())
        {
            _savedRevision = _sceneRevision;
        }
        PushSceneChanged(std::move(applied->effect.entityIds), applied->effect.full);
        PushHistoryChanged();
        NoteEditStepEnded();

        BufferWriter response;
        WriteEditResult(response, applied->label, _sceneRevision, _history.CanUndo(), _history.CanRedo(), _savedRevision);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleUndo(int clientSocket)
    {
        UndoOrRedo(clientSocket, true);
    }

    void EditorServer::HandleRedo(int clientSocket)
    {
        UndoOrRedo(clientSocket, false);
    }

    void EditorServer::HandleBeginEditGroup(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const BeginEditGroupCmd cmd = BeginEditGroupCmd::Deserialize(reader);

        std::string label = SanitizeForLog(cmd.label, 100);
        if (label.empty())
        {
            label = "Edit";
        }
        const bool outermost = !_history.InGroup();
        if (!_history.BeginGroup(std::move(label)))
        {
            SendError(clientSocket, std::format("Edit groups can't be nested more than {} deep",
                                                EditHistory::MaxGroupDepth));
            return;
        }
        if (outermost)
        {
            // CanUndo and CanRedo are false while the group is open
            PushHistoryChanged();
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleEndEditGroup(int clientSocket)
    {
        const EditHistory::GroupEnd ended = _history.EndGroup();
        if (ended == EditHistory::GroupEnd::NotOpen)
        {
            SendError(clientSocket, "No edit group is open");
            return;
        }
        if (ended == EditHistory::GroupEnd::Committed || ended == EditHistory::GroupEnd::Empty ||
            ended == EditHistory::GroupEnd::NoChange)
        {
            PushHistoryChanged();
        }
        if (ended == EditHistory::GroupEnd::Committed)
        {
            NoteEditStepEnded();
        }
        else if (ended == EditHistory::GroupEnd::NoChange)
        {
            NoteGroupWithoutChange();
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetHistory(int clientSocket)
    {
        nlohmann::json entries = nlohmann::json::array();
        for (const EditHistory::Entry &entry : _history.Entries())
        {
            entries.push_back(nlohmann::json{{"label", entry.label}, {"bytes", entry.bytes}});
        }

        BufferWriter response;
        WriteHistory(response, static_cast<uint32_t>(_history.Cursor()), entries);
        SendResponse(clientSocket, response.Release());
    }

    std::filesystem::path EditorServer::GetAutosaveFile() const
    {
        // A play host has no autosave: the files under .n2/autosave belong to the host that edits (the scene it plays
        // has no file of its own, so its autosave path would be the untitled scene's, which that host may have written)
        if (!_project || _playMode)
        {
            return {};
        }
        // A scene with a file is autosaved at the same path under .n2/autosave/ (the path was checked to be inside
        // assets/ when the scene was opened); one without, at a fixed name
        std::filesystem::path relative = ".untitled.scene";
        if (const std::string scenePath = OpenScenePath(); !scenePath.empty())
        {
            const std::filesystem::path candidate = FromUtf8(IO::ResourcePath(scenePath).GetPath()).lexically_normal();
            if (!candidate.empty() && candidate.is_relative() && !candidate.has_root_name() &&
                !candidate.has_root_directory() && *candidate.begin() != "..")
            {
                relative = candidate;
            }
        }
        return _project->root / ".n2" / "autosave" / relative;
    }

    void EditorServer::RemoveAutosaveFile(const std::filesystem::path &file)
    {
        if (file.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove(file, error);
    }

    void EditorServer::ResetAutosaveState()
    {
        _autosavePending = false;
        _lastAutosave.reset();
        _autosaveWarned = false;
        _protectedAutosaveWarned = false;
        // An autosave from before is what a client may want to offer: nothing overwrites it until it decides
        std::error_code error;
        const std::filesystem::path file = GetAutosaveFile();
        _autosaveProtected = !file.empty() && std::filesystem::is_regular_file(file, error);
    }

    void EditorServer::NoteEditStepEnded()
    {
        // Written when the command is over (ExecuteCommand), so it sees the scene as the command left it
        _autosavePending = true;
    }

    void EditorServer::PrepareForShutdown()
    {
        CloseEditGroups();
        DiscardWrittenAutosave();
    }

    void EditorServer::DiscardWrittenAutosave()
    {
        _autosavePending = false;
        if (_playMode)
        {
            return; // nothing a play host wrote, and nothing of the edit host's to remove
        }
        // No scene of this host's yet: whatever autosave is there belongs to an earlier session, not to this host
        const Scene *loaded = SceneManager::GetCurScene();
        if (!_autosaveProtected && loaded != nullptr && loaded == _openScene)
        {
            RemoveAutosaveFile(GetAutosaveFile());
        }
    }

    void EditorServer::FlushAutosave(const bool force)
    {
        if (!_autosavePending || _playMode)
        {
            return;
        }
        const Scene *scene = SceneManager::GetCurScene();
        if (!_project || scene == nullptr)
        {
            _autosavePending = false;
            return;
        }
        if (_autosaveProtected)
        {
            _autosavePending = false;
            if (!_protectedAutosaveWarned)
            {
                _protectedAutosaveWarned = true;
                Logger::Warn("An autosave from an earlier session is kept, and none is written over it, until it is "
                             "restored or discarded (RestoreAutosave, DiscardAutosave) or the scene is saved");
            }
            return;
        }
        const std::filesystem::path file = GetAutosaveFile();
        if (_sceneRevision == _savedRevision)
        {
            // Nothing unsaved (undone back to the saved state): what the file holds is out of date
            _autosavePending = false;
            RemoveAutosaveFile(file);
            return;
        }
        const auto now = _autosaveClock ? _autosaveClock() : std::chrono::steady_clock::now();
        if (!force && _lastAutosave.has_value() && now - *_lastAutosave < _autosaveInterval)
        {
            return; // still due: the host's loop flushes it once the interval has passed
        }

        _autosavePending = false;
        _lastAutosave = now;
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        const auto written = IO::WriteTextFileAtomically(file, SceneFileText(scene->Serialize()));
        if (!written && !_autosaveWarned)
        {
            // Once: a folder that can't be written would otherwise log after every edit
            _autosaveWarned = true;
            Logger::Warn("Couldn't write the autosave: " + written.error());
        }
        else if (written)
        {
            _autosaveWarned = false;
        }
    }

    void EditorServer::HandleGetAutosave(int clientSocket)
    {
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        if (SceneManager::GetCurScene() == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        nlohmann::json info{{"exists", false}};
        const std::filesystem::path file = GetAutosaveFile();
        std::error_code error;
        if (std::filesystem::is_regular_file(file, error))
        {
            info["exists"] = true;
            info["path"] = Utf8(file);
            const auto size = std::filesystem::file_size(file, error);
            info["size"] = error ? 0 : static_cast<uint64_t>(size);
            const auto modified = std::filesystem::last_write_time(file, error);
            if (!error)
            {
                const auto sinceEpoch = std::chrono::clock_cast<std::chrono::system_clock>(modified).time_since_epoch();
                info["modified"] = std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count();
            }
            else
            {
                info["modified"] = 0;
            }
        }

        BufferWriter response;
        WriteAutosave(response, info);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleRestoreAutosave(int clientSocket)
    {
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::filesystem::path file = GetAutosaveFile();
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error))
        {
            SendError(clientSocket, "There is no autosave of the open scene");
            return;
        }
        auto text = ReadTextFile(file);
        if (!text)
        {
            SendError(clientSocket, text.error());
            return;
        }

        // One scene's autosave is not another's (every scene without a file shares a name for it)
        const nlohmann::json autosaved = nlohmann::json::parse(*text, nullptr, false);
        if (autosaved.is_object() && autosaved.contains("name") && autosaved.at("name").is_string() &&
            autosaved.at("name").get<std::string>() != scene->sceneName)
        {
            SendError(clientSocket, std::format("The autosave is of a scene named '{}', not '{}'",
                                                SanitizeForLog(autosaved.at("name").get<std::string>()),
                                                SanitizeForLog(scene->sceneName)));
            return;
        }

        // The scene as it is now, for undoing; the restore builds the autosave first, so one that isn't a valid scene
        // is an Error and changes nothing
        auto before = std::make_shared<const std::string>(SnapshotScene(*scene));
        const EditOutcome restored = RestoreSceneSnapshot(*text);
        if (!restored)
        {
            SendError(clientSocket, "The autosave can't be used: " + restored.error());
            return;
        }
        auto after = std::make_shared<const std::string>(std::move(*text));

        EditOp op;
        op.undo = [this, before]() -> EditOutcome { return RestoreSceneSnapshot(*before); };
        op.redo = [this, after]() -> EditOutcome { return RestoreSceneSnapshot(*after); };
        op.undoBytes = before->size();
        op.redoBytes = after->size();

        // The restored scene differs from the file: unsaved changes, every id new
        _autosaveProtected = false;
        MarkSceneChanged({}, true);
        RecordEdit("Restore autosave", std::move(op));
        SendSceneInfo(clientSocket, GetOpenSceneInfo());
    }

    void EditorServer::HandleDiscardAutosave(int clientSocket)
    {
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        if (SceneManager::GetCurScene() == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        RemoveAutosaveFile(GetAutosaveFile());
        _autosaveProtected = false;

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Project and scene files ====================

    void EditorServer::SetProject(std::filesystem::path root, IO::ProjectFile project)
    {
        _project = ProjectState{std::move(root), std::move(project)};
    }

    std::expected<ResolvedScenePath, std::string> EditorServer::ResolveScenePath(const std::filesystem::path &assetsRoot,
                                                                                const std::string &path)
    {
        const std::string shown = SanitizeForLog(path);
        if (!IO::ProjectFile::IsScenePath(path))
        {
            return std::unexpected(std::format("Not a scene path: '{}' (expected res://<folder>/<name>.scene)", shown));
        }
        const IO::ResourcePath resourcePath(path);
        const std::filesystem::path relative = FromUtf8(resourcePath.GetPath());
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        {
            return std::unexpected(std::format("Not a scene path inside the project: '{}'", shown));
        }

        // Lexically inside the assets folder, then also with symlinks resolved for the part that exists (as
        // ResolveSceneFile does), so a link inside assets/ can't lead a write elsewhere
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(assetsRoot, error);
        if (error)
        {
            return std::unexpected("The project's assets folder can't be resolved");
        }
        const std::filesystem::path file = (root / relative).lexically_normal();
        const std::filesystem::path parent = std::filesystem::weakly_canonical(file.parent_path(), error);
        if (error)
        {
            return std::unexpected(std::format("Can't resolve the folder of '{}'", shown));
        }
        const std::filesystem::path lexical = file.lexically_relative(root);
        const std::filesystem::path resolved = parent.lexically_relative(root);
        const auto outside = [](const std::filesystem::path &fromRoot)
        {
            return fromRoot.empty() || *fromRoot.begin() == "..";
        };
        if (outside(lexical) || (resolved != "." && outside(resolved)))
        {
            return std::unexpected(std::format("Not a scene path inside the project's assets folder: '{}'", shown));
        }

        // An existing file is resolved itself too: a symlinked file can't lead a read or write out of the folder, and
        // the path takes the spelling the file system has. That spelling, relative to the folder, is the scene's
        // res:// path, so "res://Scenes/main.scene" on a case-insensitive file system is the same scene (the same
        // UUID, the same .meta) as "res://scenes/Main.scene".
        std::filesystem::path target = parent / file.filename();
        if (std::error_code existsError; std::filesystem::exists(target, existsError))
        {
            target = std::filesystem::weakly_canonical(target, error);
            if (error)
            {
                return std::unexpected(std::format("Can't resolve '{}'", shown));
            }
        }
        const std::filesystem::path fromRoot = target.lexically_relative(root);
        if (outside(fromRoot))
        {
            return std::unexpected(std::format("Not a scene path inside the project's assets folder: '{}'", shown));
        }
        IO::ResourcePath canonicalPath(IO::PathType::Resource, IO::PathToUtf8(fromRoot));
        if (!IO::ProjectFile::IsScenePath(canonicalPath.ToString()))
        {
            // A link to a file that isn't a .scene
            return std::unexpected(std::format("Not a scene file: '{}'", shown));
        }
        return ResolvedScenePath{std::move(canonicalPath), std::move(target)};
    }

    std::string EditorServer::OpenScenePath() const
    {
        const Scene *loaded = SceneManager::GetCurScene();
        return loaded != nullptr && loaded == _openScene ? _openScenePath : std::string{};
    }

    void EditorServer::PushSceneChanged(std::vector<std::string> entityIds, const bool full)
    {
        nlohmann::json event{{"revision", _sceneRevision},
                             {"savedRevision", _savedRevision},
                             {"path", OpenScenePath()}};
        if (full)
        {
            // Every id a client holds is invalid, so which ones changed doesn't matter
            event["full"] = true;
        }
        else if (!entityIds.empty() && entityIds.size() <= MaxEventEntityIds)
        {
            event["entityIds"] = std::move(entityIds);
        }
        _events.Push("sceneChanged", std::move(event));
    }

    void EditorServer::MarkSceneChanged(std::vector<std::string> entityIds, const bool full)
    {
        ++_sceneRevision;
        NoteSceneChanged();
        PushSceneChanged(std::move(entityIds), full);
    }

    void EditorServer::SendError(int clientSocket, const std::string &message)
    {
        BufferWriter response;
        WriteError(response, message);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::LoadOpenedScene(std::unique_ptr<Scene> scene, std::string path)
    {
        // The scene that was open is left: what it hadn't saved is dropped, and so is its autosave
        DiscardWrittenAutosave();

        // This very object becomes the loaded scene (edit mode: built, never attached)
        Scene *const opened = scene.get();
        opened->SetEditMode(true);
        SceneManager::AddScene(std::move(scene), true);
        SceneManager::ProcessAnyPendingSceneChange();
        if (SceneManager::GetCurScene() != opened)
        {
            throw std::runtime_error("the scene couldn't be loaded");
        }
        _openScene = opened;
        _openScenePath = std::move(path);
        ++_sceneRevision;
        NoteSceneChanged();
        _savedRevision = _sceneRevision;
        // Another scene: nothing done to the last can be undone, and the empty history is the saved state
        _history.Clear();
        _history.MarkSaved();
        ResetAutosaveState();
        // Another scene: every id a client holds is invalid
        PushSceneChanged({}, true);
        PushHistoryChanged();
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::GetOpenSceneInfo() const
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            return std::unexpected("No scene loaded");
        }
        return OpenSceneInfo{
            .path = OpenScenePath(),
            .name = scene->sceneName,
            .uuid = scene->GetUUID().ToString(),
            .revision = _sceneRevision,
            .savedRevision = _savedRevision,
        };
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::OpenSceneFile(const std::string &path)
    {
        if (!_project)
        {
            return std::unexpected("No project: scene files need a host started with --project");
        }
        auto resolved = ResolveScenePath(_project->root / "assets", path);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }
        const std::string resourcePath = resolved->resourcePath.ToString();

        std::error_code error;
        if (!std::filesystem::is_regular_file(resolved->file, error))
        {
            return std::unexpected("Scene file not found: " + SanitizeForLog(resourcePath));
        }
        auto text = ReadTextFile(resolved->file);
        if (!text)
        {
            return std::unexpected(text.error());
        }
        const nlohmann::json sceneJson = nlohmann::json::parse(*text, nullptr, false);
        if (sceneJson.is_discarded())
        {
            return std::unexpected("Not valid JSON: " + SanitizeForLog(resourcePath));
        }
        std::unique_ptr<Scene> scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            return std::unexpected("Not a valid scene (see the log): " + SanitizeForLog(resourcePath));
        }

        // The file's asset UUID, the same every run (and what a .meta records for it): a scene's UUID is its file's.
        // A file added since the last scan is indexed now.
        auto &loader = IO::ResourceLoader::Instance();
        if (!loader.Exists(resolved->resourcePath))
        {
            (void)loader.Reload(resolved->resourcePath);
        }
        scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
        scene->SetResourcePath(resolved->resourcePath);

        LoadOpenedScene(std::move(scene), resourcePath);
        Logger::Info("Opened scene " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::NewSceneFile(const std::string &path, const std::string &name)
    {
        std::optional<ResolvedScenePath> resolved;
        if (!path.empty())
        {
            if (!_project)
            {
                return std::unexpected("No project: scene files need a host started with --project");
            }
            auto checked = ResolveScenePath(_project->root / "assets", path);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            std::error_code error;
            if (std::filesystem::exists(checked->file, error))
            {
                return std::unexpected("A file already exists at " + SanitizeForLog(checked->resourcePath.ToString()) +
                                       " (open it with OpenScene)");
            }
            resolved = std::move(*checked);
        }

        std::string sceneName = name;
        if (sceneName.empty())
        {
            sceneName = resolved ? resolved->resourcePath.GetStem() : std::string("Untitled");
        }
        std::unique_ptr<Scene> scene = Scene::Create(sceneName);

        std::string resourcePath;
        if (resolved)
        {
            // Written before the switch: a file that can't be written leaves the loaded scene as it is
            std::error_code error;
            std::filesystem::create_directories(resolved->file.parent_path(), error);
            if (auto written = IO::WriteTextFileAtomically(resolved->file, SceneFileText(scene->Serialize())); !written)
            {
                return std::unexpected(written.error());
            }
            resourcePath = resolved->resourcePath.ToString();
            (void)IO::ResourceLoader::Instance().Reload(resolved->resourcePath);
            scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
            scene->SetResourcePath(resolved->resourcePath);
        }

        LoadOpenedScene(std::move(scene), resourcePath);
        Logger::Info(resourcePath.empty() ? "New scene " + SanitizeForLog(sceneName) + " (not saved to a file yet)"
                                          : "New scene " + SanitizeForLog(sceneName) + " at " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::SaveSceneFile(const std::string &path)
    {
        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            return std::unexpected("No scene loaded");
        }
        if (!_project)
        {
            return std::unexpected("No project: scene files need a host started with --project");
        }
        const std::string target = path.empty() ? OpenScenePath() : path;
        if (target.empty())
        {
            return std::unexpected("The open scene has no file yet: pass the path to save it to "
                                   "(res://<folder>/<name>.scene)");
        }
        auto resolved = ResolveScenePath(_project->root / "assets", target);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }

        nlohmann::json sceneJson = scene->Serialize();
        // The autosave of the scene as it was named before this save, which may be another file
        const std::filesystem::path previousAutosave = GetAutosaveFile();
        std::error_code error;
        std::filesystem::create_directories(resolved->file.parent_path(), error);
        if (auto written = IO::WriteTextFileAtomically(resolved->file, SceneFileText(sceneJson)); !written)
        {
            return std::unexpected(written.error());
        }
        // As SaveScene does: the stored snapshot is what loading this scene by name rebuilds
        SceneManager::UpdateScene(SceneManager::GetCurSceneIndex(), sceneJson);

        // The file is indexed (or its .meta brought up to date), and a cached copy of the old file dropped
        (void)IO::ResourceLoader::Instance().Reload(resolved->resourcePath);
        const std::string resourcePath = resolved->resourcePath.ToString();
        if (resourcePath != OpenScenePath())
        {
            // Saved as another file: that is the scene's file now, and its UUID the scene's
            scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
            scene->SetResourcePath(resolved->resourcePath);
            _openScene = scene;
            _openScenePath = resourcePath;
        }
        _savedRevision = _sceneRevision;
        // The history is kept (as in Unity): undoing back to this state is no unsaved change
        _history.MarkSaved();
        // The scene is on disk: an autosave is out of date, and one found at open is no longer a reason to wait
        RemoveAutosaveFile(previousAutosave);
        RemoveAutosaveFile(GetAutosaveFile());
        _autosavePending = false;
        _autosaveProtected = false;
        PushSceneChanged();
        Logger::Info("Saved scene " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<void, std::string> EditorServer::SaveProject(IO::ProjectFile changed)
    {
        if (auto saved = changed.Save(_project->root); !saved)
        {
            return std::unexpected("Couldn't save " + std::string(IO::ProjectFile::FileName) + ": " + saved.error());
        }
        _project->file = std::move(changed);
        _events.Push("projectChanged", nlohmann::json::object());
        return {};
    }

    void EditorServer::SendSceneInfo(int clientSocket, const std::expected<OpenSceneInfo, std::string> &info)
    {
        BufferWriter response;
        if (info)
        {
            WriteSceneInfo(response, info->path, info->name, info->uuid, info->revision, info->savedRevision);
        }
        else
        {
            WriteError(response, info.error());
        }
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::SendProjectInfo(int clientSocket)
    {
        BufferWriter response;
        WriteProjectInfo(response, Utf8(_project->root), Utf8(IO::ResourceLoader::Instance().GetUserDataRoot()),
                         _project->file.ToJson());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::SendNoProject(int clientSocket)
    {
        BufferWriter response;
        WriteError(response, "No project: the host wasn't started with --project");
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleOpenScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const OpenSceneCmd cmd = OpenSceneCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, OpenSceneFile(cmd.path));
    }

    void EditorServer::HandleSaveSceneToFile(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SaveSceneToFileCmd cmd = SaveSceneToFileCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, SaveSceneFile(cmd.path));
    }

    void EditorServer::HandleNewScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const NewSceneCmd cmd = NewSceneCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, NewSceneFile(cmd.path, cmd.name));
    }

    void EditorServer::HandleGetOpenScene(int clientSocket)
    {
        SendSceneInfo(clientSocket, GetOpenSceneInfo());
    }

    void EditorServer::HandleGetProjectInfo(int clientSocket)
    {
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        SendProjectInfo(clientSocket);
    }

    void EditorServer::HandleSetProjectSettings(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetProjectSettingsCmd cmd = SetProjectSettingsCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }

        BufferWriter response;
        if (!cmd.settings.is_object())
        {
            WriteError(response, "settings must be a JSON object: a merge patch (RFC 7386) for the project's settings");
            SendResponse(clientSocket, response.Release());
            return;
        }

        // A merge patch: keys set to null are removed, objects merge recursively, anything else replaces
        nlohmann::json merged = _project->file.settings;
        merged.merge_patch(cmd.settings);

        // Only the blocks the patch touches are applied, live, before saving; the state they change is captured first,
        // so a refusal (or a failed save) puts back exactly what was running, including a block the previous settings
        // never had
        std::set<std::string> touched;
        for (const auto &[key, value] : cmd.settings.items())
        {
            touched.insert(key);
        }
        const ProjectSettingsSnapshot before = ProjectSettingsSnapshot::Capture();
        // Applied settings can change the picture (rendering.colorSpace, the lighting), applied or restored
        NoteViewChanged();
        if (const std::vector<std::string> problems = ApplyProjectSettings(merged, touched); !problems.empty())
        {
            before.Restore();
            std::string message = "Project settings refused:";
            for (const std::string &problem : problems)
            {
                message += " " + problem + ";";
            }
            message.pop_back();
            WriteError(response, message);
            SendResponse(clientSocket, response.Release());
            return;
        }

        IO::ProjectFile changed = _project->file;
        changed.settings = std::move(merged);
        if (auto saved = SaveProject(std::move(changed)); !saved)
        {
            before.Restore();
            WriteError(response, saved.error());
            SendResponse(clientSocket, response.Release());
            return;
        }
        Logger::Info("Project settings changed and saved");
        SendProjectInfo(clientSocket);
    }

    void EditorServer::HandleSetStartupScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetStartupSceneCmd cmd = SetStartupSceneCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }

        BufferWriter response;
        IO::ProjectFile changed = _project->file;
        if (cmd.path.empty())
        {
            changed.startupScene.clear();
        }
        else
        {
            const auto resolved = ResolveScenePath(_project->root / "assets", cmd.path);
            std::error_code error;
            if (!resolved || !std::filesystem::is_regular_file(resolved->file, error))
            {
                WriteError(response, resolved ? "Scene file not found: " + SanitizeForLog(cmd.path) : resolved.error());
                SendResponse(clientSocket, response.Release());
                return;
            }
            // Normalised, and added to the scene list if it isn't there
            changed.startupScene = resolved->resourcePath.ToString();
            if (std::ranges::find(changed.scenes, changed.startupScene) == changed.scenes.end())
            {
                changed.scenes.push_back(changed.startupScene);
            }
        }

        if (auto saved = SaveProject(std::move(changed)); !saved)
        {
            WriteError(response, saved.error());
            SendResponse(clientSocket, response.Release());
            return;
        }
        Logger::Info(_project->file.startupScene.empty() ? std::string("The project has no startup scene now")
                                                         : "Startup scene: " + _project->file.startupScene);
        SendProjectInfo(clientSocket);
    }

    bool EditorServer::Send(int socket, const void *data, size_t size)
    {
        size_t sent = 0;
        auto *bytes = static_cast<const char*>(data);

        while (sent < size)
        {
            if (!WaitUntilReady(socket, true)) return false;
            int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
            if (result <= 0) return false;
            sent += result;
        }
        return true;
    }

    bool EditorServer::Receive(int socket, void *data, size_t size,
                               std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        size_t received = 0;
        auto *bytes = static_cast<char*>(data);

        while (received < size)
        {
            if (!WaitUntilReady(socket, false, deadline)) return false;
            int result = recv(socket, bytes + received, static_cast<int>(size - received), 0);
            if (result <= 0) return false;
            received += result;
        }
        return true;
    }

    bool EditorServer::WaitUntilReady(int sock, bool forWrite,
                                      std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        const auto nativeSocket = static_cast<NativeSocket>(sock);
        while (_running)
        {
            if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline)
                return false;

            fd_set set;
            FD_ZERO(&set);
            FD_SET(nativeSocket, &set);
            timeval timeout{0, PollIntervalMicroseconds};

            // The first argument is ignored on Windows
            const int result = select(sock + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &timeout);
            if (result > 0) return true;
            if (result < 0) return false;
        }
        return false;
    }

    void EditorServer::SendResponse(int /*clientSocket*/, std::vector<uint8_t> data)
    {
        // Main thread: recorded for ExecuteCommand to return; the network thread does the sending.
        // By value and moved, since a frame can be tens of MiB.
        _response = std::move(data);
    }
}
