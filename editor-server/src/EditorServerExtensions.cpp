// InvokeMenuItem (protocol 1.15.0), InvokeInspectorAction (1.16.0) and the nested dispatch the scripts of editor
// extensions reach the editor through (design: lua-editor-extensions sections 4.2, 4.3, 6 and 7.1). Both commands run
// through one function (RunExtensionCall). The ExtensionHost runs the script on a worker thread and
// hands every Editor.Scene or Editor.Log call to the handler built here, which runs on the command thread: so every scene
// change an extension makes is a command, sent through EditorServer::ProcessCommand like a client's, with that client's
// role, the read-only gate and the audit line, inside one edit group.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/serialization/FieldInfo.hpp"

#include "editor-server/ClientText.hpp"
#include "editor-server/Commands.hpp"
#include "editor-server/EditorServer.hpp"
#include "editor-server/ExtensionHost.hpp"
#include "editor-server/Protocol.hpp"
#include "editor-server/ServerLog.hpp"

namespace N2Engine::Editor
{
    using namespace Protocol;
    using nlohmann::json;

    // ==================== What a script may send ====================

    namespace
    {
        struct NestedRule
        {
            CommandType type;
            bool allowed;
        };

        // Every command of protocol.json, decided. An allow list in effect (an id that is not here is denied too), written
        // as a table of both so that a command added later has to be decided: ProtocolSpecTests requires every command of
        // the spec to be listed.
        constexpr NestedRule NestedTable[] = {
            // What Editor.Scene reads with (scene.read)
            {CommandType::GetHierarchy, true},
            {CommandType::GetEntity, true},
            {CommandType::GetComponentTypes, true},
            {CommandType::GetComponent, true},
            // What Editor.Scene changes the scene with (scene.write): each records its edit in the open group
            {CommandType::CreateEntityEx, true},
            {CommandType::DestroyEntity, true},
            {CommandType::DuplicateEntity, true},
            {CommandType::SetEntityParent, true},
            {CommandType::SetEntityProperties, true},
            {CommandType::SetLocalTransform, true},
            {CommandType::AddComponent, true},
            {CommandType::RemoveComponent, true},
            {CommandType::SetComponentFields, true},

            // The session and the host: Hello binds an identity and closes the groups; Shutdown stops the host
            {CommandType::Hello, false},
            {CommandType::Shutdown, false},
            {CommandType::PollEvents, false},
            // Extension commands: no recursion, no reload from inside a call
            {CommandType::ListEditorExtensions, false},
            {CommandType::GetEditorUi, false},
            {CommandType::InvokeMenuItem, false},
            {CommandType::InvokeInspectorAction, false},
            {CommandType::ReloadEditorExtensions, false},
            // The open scene and the project's files: opening, loading, creating, saving and deleting scenes
            {CommandType::CreateScene, false},
            {CommandType::LoadScene, false},
            {CommandType::SaveScene, false},
            {CommandType::DeleteScene, false},
            {CommandType::GetCurrentScene, false},
            {CommandType::OpenScene, false},
            {CommandType::SaveSceneToFile, false},
            {CommandType::NewScene, false},
            {CommandType::GetOpenScene, false},
            {CommandType::GetProjectInfo, false},
            {CommandType::SetProjectSettings, false},
            {CommandType::SetStartupScene, false},
            // The history: an extension's step is one group, made by the host; it may not undo, redo or group on its own
            {CommandType::Undo, false},
            {CommandType::Redo, false},
            {CommandType::BeginEditGroup, false},
            {CommandType::EndEditGroup, false},
            {CommandType::GetHistory, false},
            {CommandType::GetAutosave, false},
            {CommandType::RestoreAutosave, false},
            {CommandType::DiscardAutosave, false},
            // Play: a snapshot is written for another process, and the keys and the clock belong to a play host
            {CommandType::WritePlaySnapshot, false},
            {CommandType::SetPaused, false},
            {CommandType::Step, false},
            {CommandType::GetPlayState, false},
            {CommandType::SendInput, false},
            // The viewport and the camera are the viewer's
            {CommandType::RenderFrame, false},
            {CommandType::SetViewportSize, false},
            {CommandType::GetAudio, false},
            {CommandType::RenderFrameIfChanged, false},
            {CommandType::SetCameraPosition, false},
            {CommandType::GetCameraPosition, false},
            {CommandType::SetEditorCamera, false},
            {CommandType::GetEditorCamera, false},
            {CommandType::PickEntity, false},
            {CommandType::GetEntityBounds, false},
            // Entities and components beyond what Editor.Scene has: the older commands and the readers it does not use
            {CommandType::CreateEntity, false},
            {CommandType::SetEntityTransform, false},
            {CommandType::GetEntityTransform, false},
            {CommandType::GetAllEntities, false},
            {CommandType::GetLuaFields, false},
            // Assets: assets.read and assets.write are a later capability (E-X6)
            {CommandType::CreateScript, false},
            {CommandType::RescanAssets, false},
            {CommandType::ListAssets, false},
            {CommandType::GetAssetInfo, false},
            {CommandType::SetImportSettings, false},
            {CommandType::ReadTextAsset, false},
            {CommandType::WriteTextAsset, false},
            {CommandType::CreateScriptAsset, false},
            {CommandType::CreateFolder, false},
            {CommandType::GetEngineHealth, false},
        };
    }

    std::optional<bool> EditorServer::NestedCommandPolicy(const uint8_t commandType)
    {
        for (const NestedRule &rule : NestedTable)
        {
            if (static_cast<uint8_t>(rule.type) == commandType)
            {
                return rule.allowed;
            }
        }
        return std::nullopt;
    }

    // ==================== The nested dispatch ====================

    std::vector<uint8_t> EditorServer::DispatchNested(const uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        const std::string name = CommandName(commandType);
        const std::string via = _invocation != nullptr ? _invocation->via : std::string("test");
        const auto refuse = [](const std::string &text)
        {
            BufferWriter response;
            WriteError(response, text);
            return response.Release();
        };

        const std::optional<bool> policy = NestedCommandPolicy(commandType);
        if (!policy.value_or(false))
        {
            ServerLog::Warn(std::format("audit: extension {} sent {}, which an editor extension may not send (refused)", via,
                                     name));
            return refuse(std::format("An editor extension may not send {}", name));
        }
        if (_invocation != nullptr && ++_invocation->sent > ExtensionHost::MaxNested())
        {
            return refuse(std::format("An editor extension may send at most {} commands in one call",
                                      ExtensionHost::MaxNested()));
        }

        // The outer command's answer is not touched: ExecuteCommand clears _response on entry and flushes the autosave on
        // exit, so it is not called here; ProcessCommand is, with the identity of the connection being served
        std::vector<uint8_t> saved = std::exchange(_response, {});
        bool failed = false;
        std::string failure;
        try
        {
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
        std::vector<uint8_t> frame = std::exchange(_response, std::move(saved));

        if (failed)
        {
            // What an exception said is the host's (a std::filesystem_error names its paths): the log has it, a remote
            // client is told that the command failed
            ServerLog::Error(std::format("Command 0x{:X} sent by extension {} failed: {}", commandType, via, failure));
            return refuse("Command failed: " + (ClientText::IsLocal() ? SanitizeForLog(failure, 300)
                                                                      : std::string("the host couldn't complete it (see the "
                                                                                    "host's log)")));
        }
        if (frame.empty())
        {
            return refuse("The command produced no response");
        }
        return frame;
    }

    std::vector<uint8_t> EditorServer::DispatchNestedForTest(const uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        ExtensionInvocation test;
        test.via = "test";
        test.name = "test";
        ExtensionInvocation *const previous = std::exchange(_invocation, &test);
        std::vector<uint8_t> frame;
        try
        {
            frame = DispatchNested(commandType, payload);
        }
        catch (...)
        {
            _invocation = previous;
            throw;
        }
        _invocation = previous;
        return frame;
    }

    // ==================== What a script asks for ====================

    namespace
    {
        struct NestedFrame
        {
            uint8_t type = 0;
            std::vector<uint8_t> payload;
        };

        NestedFrame ParseNestedFrame(const std::vector<uint8_t> &frame)
        {
            BufferReader reader(frame);
            NestedFrame parsed;
            parsed.type = reader.ReadU8();
            const std::span<const uint8_t> body = reader.ReadBytes(reader.ReadU32());
            parsed.payload.assign(body.begin(), body.end());
            return parsed;
        }

        // A float of the wire from a number of the script (an out-of-range one becomes infinity, which the host's own
        // check for a finite transform refuses)
        float WireFloat(const json &value)
        {
            return static_cast<float>(value.get<double>());
        }

        // An object { x, y, z } (and w when `withW`) of numbers, or the reason it is not one
        void CheckVector(const json &value, const char *what, const bool withW)
        {
            const bool shaped = value.is_object() && value.contains("x") && value.contains("y") && value.contains("z") &&
                                value.at("x").is_number() && value.at("y").is_number() && value.at("z").is_number() &&
                                (!withW || (value.contains("w") && value.at("w").is_number()));
            if (!shaped)
            {
                throw std::runtime_error(std::format("'{}' must be a table {} of numbers", what,
                                                     withW ? "{ x = .., y = .., z = .., w = .. }" : "{ x = .., y = .., z = .. }"));
            }
        }

        // The keys of a table that are not in `allowed`, as an error
        void RejectOtherKeys(const json &table, std::initializer_list<std::string_view> allowed, const char *what)
        {
            for (const auto &[key, value] : table.items())
            {
                (void)value;
                if (std::ranges::find(allowed, std::string_view(key)) == allowed.end())
                {
                    throw std::runtime_error(std::format("{} has no field '{}'", what, EditorServer::SanitizeForLog(key, 64)));
                }
            }
        }

        // A list that an empty table of the script stands for too
        bool IsListOrEmpty(const json &value)
        {
            return value.is_array() || (value.is_object() && value.empty());
        }
    }

    json EditorServer::ExtensionRequest(const std::string &op, const json &args, std::string &error)
    {
        const auto granted = [this](const std::string_view capability)
        {
            return _invocation != nullptr &&
                   std::ranges::find(_invocation->capabilities, std::string(capability)) != _invocation->capabilities.end();
        };

        try
        {
            if (op == "log")
            {
                if (!args.is_object())
                {
                    throw std::runtime_error("a log call needs a level and a text");
                }
                std::string level = args.value("level", std::string("info"));
                if (level != "info" && level != "warn" && level != "error")
                {
                    level = "info";
                }
                const std::string text = SanitizeForLog(args.value("text", std::string()), ExtensionHost::MaxLogLineBytes);
                const std::string name = _invocation != nullptr ? _invocation->name : std::string("extension");
                // Admin-tagged, like the host's own lines: the output is project code's and a viewer or an editor reads
                // none of it (audit F9); the same line is a log line, so an old client's console shows it too
                _events.Push("extensionOutput",
                             json{{"extension", name},
                                  {"level", level},
                                  {"message", text},
                                  {"time", EventRing::NowUnixMilliseconds()}},
                             Logger::LogSensitivity::Admin);
                const std::string line = std::format("extension {}: {}", name, text);
                if (level == "warn")
                {
                    ServerLog::Warn(line);
                }
                else if (level == "error")
                {
                    ServerLog::Error(line);
                }
                else
                {
                    ServerLog::Info(line);
                }
                return nullptr;
            }
            if (!op.starts_with("scene."))
            {
                throw std::runtime_error("the host has no operation " + SanitizeForLog(op, 64));
            }
            if (!args.is_null() && !args.is_object())
            {
                throw std::runtime_error("the arguments of a call must be a table");
            }
            const json noArgs = json::object();
            const json &a = args.is_null() ? noArgs : args;

            const auto str = [&a](const char *key) -> std::string
            {
                const auto found = a.find(key);
                if (found == a.end() || !found->is_string())
                {
                    throw std::runtime_error(std::format("'{}' must be a string", key));
                }
                return found->get<std::string>();
            };
            // A string that may be left out (nil is the same as absent)
            const auto optionalStr = [&a](const char *key) -> std::string
            {
                const auto found = a.find(key);
                if (found == a.end() || found->is_null())
                {
                    return {};
                }
                if (!found->is_string())
                {
                    throw std::runtime_error(std::format("'{}' must be a string", key));
                }
                return found->get<std::string>();
            };
            // Sends one command; its answer's payload, or the host's refusal as the error of the call
            const auto send = [this](const CommandType type, const BufferWriter &request,
                                     const ResponseType expected) -> std::vector<uint8_t>
            {
                const std::span<const uint8_t> data = request.Data();
                const std::vector<uint8_t> frame =
                    DispatchNested(static_cast<uint8_t>(type), std::vector<uint8_t>(data.begin(), data.end()));
                const NestedFrame parsed = ParseNestedFrame(frame);
                if (parsed.type == static_cast<uint8_t>(ResponseType::Error))
                {
                    throw std::runtime_error(std::string(parsed.payload.begin(), parsed.payload.end()));
                }
                if (parsed.type == static_cast<uint8_t>(ResponseType::ErrorCoded))
                {
                    BufferReader reader(parsed.payload);
                    throw std::runtime_error(reader.ReadString());
                }
                if (parsed.type != static_cast<uint8_t>(expected))
                {
                    throw std::runtime_error("the host answered with something the call did not expect");
                }
                return parsed.payload;
            };
            const auto sendWithId = [&send](const CommandType type, const std::string &id, const ResponseType expected)
            {
                BufferWriter request;
                request.WriteString(id);
                return send(type, request, expected);
            };
            const auto hierarchy = [&send]() -> json
            {
                const std::vector<uint8_t> body = send(CommandType::GetHierarchy, BufferWriter{}, ResponseType::Hierarchy);
                BufferReader reader(body);
                (void)reader.ReadU32(); // the scene revision
                return ReadJson(reader);
            };
            const auto entityOf = [&sendWithId](const std::string &id) -> json
            {
                const std::vector<uint8_t> body = sendWithId(CommandType::GetEntity, id, ResponseType::EntityData);
                BufferReader reader(body);
                return ReadJson(reader); // the matrix after it is not given to a script
            };
            const auto setFields = [&send](const std::string &entityId, const std::string &componentId,
                                           const json &values) -> json
            {
                BufferWriter request;
                request.WriteString(entityId);
                request.WriteString(componentId);
                WriteJson(request, values);
                const std::vector<uint8_t> body = send(CommandType::SetComponentFields, request, ResponseType::ComponentData);
                BufferReader reader(body);
                return ReadJson(reader);
            };
            // Adds a component, and sets its fields when values are given; the new component's id
            const auto addComponent = [&send, &setFields](const std::string &entityId, const std::string &typeName,
                                                          const json &values) -> std::string
            {
                BufferWriter request;
                request.WriteString(entityId);
                request.WriteString(typeName);
                const std::vector<uint8_t> body = send(CommandType::AddComponent, request, ResponseType::ComponentAdded);
                BufferReader reader(body);
                const std::string componentId = reader.ReadString();
                if (values.is_object() && !values.empty())
                {
                    (void)setFields(entityId, componentId, values);
                }
                return componentId;
            };
            // A call that sends several writes (CreateEntity with components, AddComponent with values) is all or
            // nothing: when a later command is refused, what the earlier ones recorded in the invocation's group is
            // taken back before the error reaches the script, so a script that catches it finds the scene as before
            // the call. If that cannot be done exactly, the whole invocation fails (its group is taken back at the end).
            const auto allOrNothing = [this](const auto &body) -> json
            {
                if (!_history.InGroup())
                {
                    return body();
                }
                const size_t mark = _history.MarkGroup();
                try
                {
                    return body();
                }
                catch (const std::exception &)
                {
                    const size_t recorded = _history.GroupOpCount();
                    const std::expected<EditEffect, std::string> taken = _history.TakeBackGroupTo(mark);
                    if (!taken)
                    {
                        if (!_history.InGroup())
                        {
                            // An op could not be undone and the history was cleared: a client refetches everything
                            MarkSceneChanged({}, true);
                            PushHistoryChanged();
                        }
                        if (_invocation != nullptr && _invocation->fatal.empty())
                        {
                            _invocation->fatal = SanitizeForLog(taken.error(), 300);
                        }
                    }
                    else if (recorded > mark)
                    {
                        EditEffect effect = *taken;
                        if (effect.inexact)
                        {
                            _history.ForgetSaved();
                        }
                        MarkSceneChanged(std::move(effect.entityIds), effect.full);
                    }
                    throw;
                }
            };

            // Reads need scene.read, changes scene.write. A state without the capability has no such function at all (the
            // prelude does not install it); this is the host's own check, in case a call reaches it anyway.
            const bool isRead = op == "scene.getHierarchy" || op == "scene.getEntity" || op == "scene.getComponent" ||
                                op == "scene.getLocalTransform" || op == "scene.findByName" ||
                                op == "scene.getComponentTypes";
            if (isRead && !granted("scene.read"))
            {
                throw std::runtime_error("the extension did not declare the capability scene.read");
            }
            if (!isRead && !granted("scene.write"))
            {
                throw std::runtime_error("the extension did not declare the capability scene.write");
            }
            if (!isRead && _invocation != nullptr && !_invocation->fatal.empty())
            {
                // A multi-command call could not be taken back alone, so the invocation fails as a whole: a write now
                // (the history may have been cleared, with no group to take it back) must not outlive that answer
                throw std::runtime_error("the call can no longer change the scene: " + _invocation->fatal);
            }

            if (op == "scene.getHierarchy")
            {
                return hierarchy();
            }
            if (op == "scene.getEntity")
            {
                return entityOf(str("id"));
            }
            if (op == "scene.getLocalTransform")
            {
                const json entity = entityOf(str("id"));
                if (entity.contains("transform"))
                {
                    return entity.at("transform");
                }
                // An object without a transform is at the origin of its parent
                return json{{"position", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
                            {"rotation", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}},
                            {"scale", {{"x", 1.0}, {"y", 1.0}, {"z", 1.0}}}};
            }
            if (op == "scene.findByName")
            {
                const std::string name = str("name");
                json ids = json::array();
                for (const json &node : hierarchy())
                {
                    if (node.value("name", std::string()) == name)
                    {
                        ids.push_back(node.at("id"));
                    }
                }
                return ids;
            }
            if (op == "scene.getComponent")
            {
                BufferWriter request;
                request.WriteString(str("entityId"));
                request.WriteString(str("componentId"));
                const std::vector<uint8_t> body = send(CommandType::GetComponent, request, ResponseType::ComponentData);
                BufferReader reader(body);
                return ReadJson(reader);
            }
            if (op == "scene.getComponentTypes")
            {
                const std::vector<uint8_t> body =
                    send(CommandType::GetComponentTypes, BufferWriter{}, ResponseType::ComponentTypes);
                BufferReader reader(body);
                return ReadJson(reader);
            }

            if (op == "scene.createEntity")
            {
                RejectOtherKeys(a, {"name", "parent", "components"}, "the table given to Editor.Scene.CreateEntity");
                return allOrNothing([&]() -> json
                {
                    BufferWriter request;
                    request.WriteString(optionalStr("name"));
                    request.WriteString(optionalStr("parent"));
                    request.WriteI32(-1); // the last place among its siblings
                    request.WriteString(std::string()); // the empty preset
                    const std::vector<uint8_t> body = send(CommandType::CreateEntityEx, request, ResponseType::EntityCreated);
                    BufferReader reader(body);
                    const std::string id = reader.ReadString();
                    if (const auto components = a.find("components"); components != a.end() && !components->is_null())
                    {
                        if (!IsListOrEmpty(*components))
                        {
                            throw std::runtime_error("'components' must be a list of component type names");
                        }
                        for (const json &component : *components)
                        {
                            if (component.is_string())
                            {
                                (void)addComponent(id, component.get<std::string>(), json::object());
                            }
                            else if (component.is_object() && component.contains("type") && component.at("type").is_string())
                            {
                                RejectOtherKeys(component, {"type", "values"}, "a component of Editor.Scene.CreateEntity");
                                (void)addComponent(id, component.at("type").get<std::string>(),
                                                   component.contains("values") ? component.at("values") : json::object());
                            }
                            else
                            {
                                throw std::runtime_error("'components' entries must be type names, or tables { type = .., values = .. }");
                            }
                        }
                    }
                    return id;
                });
            }
            if (op == "scene.destroy")
            {
                (void)sendWithId(CommandType::DestroyEntity, str("id"), ResponseType::Ok);
                return nullptr;
            }
            if (op == "scene.duplicate")
            {
                const std::vector<uint8_t> body =
                    sendWithId(CommandType::DuplicateEntity, str("id"), ResponseType::EntityCreated);
                BufferReader reader(body);
                return reader.ReadString();
            }
            if (op == "scene.setParent")
            {
                int32_t index = -1; // the last place among the new siblings
                if (const auto found = a.find("index"); found != a.end() && !found->is_null())
                {
                    if (!found->is_number_integer())
                    {
                        throw std::runtime_error("'index' must be an integer");
                    }
                    index = static_cast<int32_t>(std::clamp<std::int64_t>(found->get<std::int64_t>(), -1, 1 << 30));
                }
                BufferWriter request;
                request.WriteString(str("id"));
                request.WriteString(optionalStr("parent")); // nil: a root of the scene
                request.WriteI32(index);
                request.WriteBool(true); // the object keeps its place in the world
                (void)send(CommandType::SetEntityParent, request, ResponseType::Ok);
                return nullptr;
            }
            if (op == "scene.setProperties")
            {
                const auto found = a.find("properties");
                if (found == a.end() || !(found->is_object() || found->is_null()))
                {
                    throw std::runtime_error("'properties' must be a table of name, tag, layer and active");
                }
                BufferWriter request;
                request.WriteString(str("id"));
                WriteJson(request, *found);
                (void)send(CommandType::SetEntityProperties, request, ResponseType::Ok);
                return nullptr;
            }
            if (op == "scene.setLocalTransform")
            {
                const auto found = a.find("transform");
                if (found == a.end() || !found->is_object())
                {
                    throw std::runtime_error("'transform' must be a table of position, rotation and scale");
                }
                RejectOtherKeys(*found, {"position", "rotation", "scale"}, "a transform");
                const std::string id = str("id");
                // What the transform does not name keeps its value: the object's own is read first
                const json current = entityOf(id).value("transform", json::object());
                const auto part = [&](const char *key, const bool withW, const json &identity) -> json
                {
                    if (const auto given = found->find(key); given != found->end())
                    {
                        CheckVector(*given, key, withW);
                        return *given;
                    }
                    return current.contains(key) ? current.at(key) : identity;
                };
                const json position = part("position", false, json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}});
                const json rotation = part("rotation", true, json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}});
                const json scale = part("scale", false, json{{"x", 1.0}, {"y", 1.0}, {"z", 1.0}});
                BufferWriter request;
                request.WriteString(id);
                for (const char *axis : {"x", "y", "z"})
                {
                    request.WriteF32(WireFloat(position.at(axis)));
                }
                for (const char *axis : {"x", "y", "z", "w"})
                {
                    request.WriteF32(WireFloat(rotation.at(axis)));
                }
                for (const char *axis : {"x", "y", "z"})
                {
                    request.WriteF32(WireFloat(scale.at(axis)));
                }
                (void)send(CommandType::SetLocalTransform, request, ResponseType::Ok);
                return nullptr;
            }
            if (op == "scene.addComponent")
            {
                const auto values = a.find("values");
                if (values != a.end() && !values->is_null() && !values->is_object())
                {
                    throw std::runtime_error("'values' must be a table of field values");
                }
                const std::string entityId = str("id");
                const std::string typeName = str("type");
                const json given = values != a.end() && values->is_object() ? *values : json::object();
                return allOrNothing([&]() -> json { return addComponent(entityId, typeName, given); });
            }
            if (op == "scene.removeComponent")
            {
                BufferWriter request;
                request.WriteString(str("entityId"));
                request.WriteString(str("componentId"));
                (void)send(CommandType::RemoveComponent, request, ResponseType::Ok);
                return nullptr;
            }
            if (op == "scene.setFields")
            {
                const auto values = a.find("values");
                if (values == a.end() || !values->is_object())
                {
                    throw std::runtime_error("'values' must be a table of field values");
                }
                return setFields(str("entityId"), str("componentId"), *values);
            }
            throw std::runtime_error("the host has no operation " + SanitizeForLog(op, 64));
        }
        catch (const std::exception &e)
        {
            error = SanitizeForLog(e.what(), 300);
            if (error.empty())
            {
                error = "the call failed";
            }
            return nullptr;
        }
    }

    // ==================== InvokeMenuItem and InvokeInspectorAction ====================

    // What one of the two invoke commands runs: everything after the item was found and its own request checked goes
    // through RunExtensionCall, so a menu item and an inspector button share the edit group, the nested dispatch with the
    // invoker's role, the budgets, the rollback and the answer
    struct EditorServer::ExtensionCall
    {
        const ExtensionHost::MenuItemRef *item = nullptr;
        /// The command's name (InvokeMenuItem or InvokeInspectorAction), for the audit line of a refusal
        std::string command;
        /// Runs the item or the button through the extension host with the handler it is given
        std::function<ExtensionHost::InvokeOutcome(const ExtensionHost::NestedHandler &)> invoke;
    };

    namespace
    {
        // "menu item" or "inspector button", for the texts a client and the log get
        std::string NounOf(const ExtensionHost::MenuItemRef &item)
        {
            return item.inspectorAction ? "inspector button" : "menu item";
        }

        // The field of a form's schema (the FieldSchema the host built when the file loaded) as the engine's FieldInfo, so
        // the same ValidateFieldValue and ClampFieldValue that check a component's fields check the form's
        FieldInfo FieldOfSchema(const json &schema)
        {
            FieldInfo info;
            info.name = schema.at("name").get<std::string>();
            info.displayName = schema.value("displayName", info.name);
            info.typeName = schema.value("typeName", std::string("json"));
            const std::string kind = schema.at("kind").get<std::string>();
            if (kind == "Float")
            {
                info.kind = FieldKind::Float;
            }
            else if (kind == "Int")
            {
                info.kind = FieldKind::Int;
            }
            else if (kind == "String")
            {
                info.kind = FieldKind::String;
            }
            else if (kind == "Bool")
            {
                info.kind = FieldKind::Bool;
            }
            else if (kind == "Vector3")
            {
                info.kind = FieldKind::Vector3;
            }
            else if (kind == "GameObjectRef")
            {
                info.kind = FieldKind::GameObjectRef;
            }
            else if (kind == "Enum")
            {
                info.kind = FieldKind::Enum;
                info.enumOptions = schema.value("enumOptions", std::vector<std::string>{});
            }
            if (schema.contains("min") || schema.contains("max"))
            {
                const double low = schema.contains("min") ? schema.at("min").get<double>() : info.limits.first;
                const double high = schema.contains("max") ? schema.at("max").get<double>() : info.limits.second;
                info.range = std::make_pair(low, high);
            }
            return info;
        }

        // The values a form was filled with, checked against it: every name a field of the form, each value valid for
        // its field (ValidateFieldValue) and clamped to its range (ClampFieldValue), a field left out given its default.
        // The object the script receives as `params`, or why it cannot be. The same for a menu item's form and an
        // inspector button's.
        std::expected<json, std::string> CheckInvokeParams(const ExtensionHost::MenuItemRef &item, const json &given)
        {
            const char *owner = item.inspectorAction ? "button" : "item";
            if (!given.is_null() && !given.is_object())
            {
                return std::unexpected(std::format("params must be a JSON object (the values of the {}'s form)", owner));
            }
            const json none = json::object();
            const json &values = given.is_null() ? none : given;
            if (item.form.is_null() || !item.form.contains("fields"))
            {
                if (!values.empty())
                {
                    return std::unexpected(std::format("The {} '{}' takes no parameters", NounOf(item), item.itemId));
                }
                return json::object();
            }

            std::vector<FieldInfo> fields;
            std::string names;
            for (const json &schema : item.form.at("fields"))
            {
                fields.push_back(FieldOfSchema(schema));
                names += (names.empty() ? "" : ", ") + fields.back().name;
            }
            for (const auto &[key, value] : values.items())
            {
                (void)value;
                const std::string name = key;
                if (std::ranges::none_of(fields, [&name](const FieldInfo &field) { return field.name == name; }))
                {
                    return std::unexpected(std::format("Unknown parameter '{}' (the {}'s form has: {})",
                                                       EditorServer::SanitizeForLog(name, 64), owner, names));
                }
            }
            json result = json::object();
            const json &defaults = item.form.at("defaults");
            for (const FieldInfo &field : fields)
            {
                if (values.contains(field.name))
                {
                    const json &value = values.at(field.name);
                    if (const std::optional<std::string> problem = ValidateFieldValue(field, value))
                    {
                        return std::unexpected(EditorServer::SanitizeForLog(*problem, 300));
                    }
                    result[field.name] = ClampFieldValue(field, value);
                }
                else
                {
                    result[field.name] = defaults.contains(field.name) ? defaults.at(field.name) : json(nullptr);
                }
            }
            return result;
        }

        struct FoundComponent
        {
            /// The UUID string of the object that has the component
            std::string entityId;
            /// Component::GetTypeName of the component
            std::string typeName;
        };

        // The component of the open scene with this UUID string, wherever it is, or the message that says why not
        std::expected<FoundComponent, std::string> FindSceneComponent(const std::string &componentId)
        {
            Scene *scene = SceneManager::GetCurScene();
            if (scene == nullptr)
            {
                return std::unexpected(std::string("No scene loaded"));
            }
            const auto uuid = Math::UUID::FromString(componentId);
            if (!uuid.has_value())
            {
                return std::unexpected("Component not found: " + EditorServer::SanitizeForLog(componentId));
            }
            std::optional<FoundComponent> found;
            scene->TraverseAll([&found, &uuid](const std::shared_ptr<GameObject> &gameObject)
            {
                if (found.has_value())
                {
                    return;
                }
                for (const auto &component : gameObject->GetAllComponents())
                {
                    if (component->GetUUID() == uuid.value())
                    {
                        found = FoundComponent{gameObject->GetUUID().ToString(), component->GetTypeName()};
                        return;
                    }
                }
            });
            if (!found.has_value())
            {
                return std::unexpected("Component not found: " + EditorServer::SanitizeForLog(componentId));
            }
            return *found;
        }
    }

    bool EditorServer::PassExtensionGates(int clientSocket, const uint32_t uiRevision)
    {
        // The gates, in the order a client can act on them. Nothing here runs a line of Lua.
        if (!_extensions)
        {
            SendResponse(clientSocket, BuildDenial(ErrorCodeExtensionsDisabled,
                                                   "Editor extensions are not enabled on this host (it was not started "
                                                   "with --editor-extensions)"));
            return false;
        }
        if (uiRevision != _extensions->UiRevision())
        {
            SendResponse(clientSocket,
                         BuildDenial(ErrorCodeStaleUiRevision,
                                     std::format("The editor extensions changed (the request was built for UI revision {}, "
                                                 "the host is at {}): fetch GetEditorUi again",
                                                 uiRevision, _extensions->UiRevision())));
            return false;
        }
        return true;
    }

    bool EditorServer::PassExtensionCallGates(int clientSocket, const ExtensionCall &call)
    {
        const ExtensionHost::MenuItemRef &item = *call.item;
        const std::string noun = NounOf(item);
        const bool hasRoles = RequiresAccessToken();
        const ConnectionIdentity identity = CurrentIdentity();
        if (item.minRole == "admin" && hasRoles && identity.role != TokenRole::Admin)
        {
            ServerLog::Warn(std::format("audit: denied user={} role={} command={} {}={}{}", identity.user,
                                     TokenRoleName(identity.role), call.command, item.inspectorAction ? "action" : "item",
                                     item.itemId, PeerSuffix()));
            SendResponse(clientSocket,
                         BuildDenial(ErrorCodeForbidden,
                                     std::format("forbidden: the {} {} may only be run by an admin (role {})", noun,
                                                 item.itemId, TokenRoleName(identity.role))));
            return false;
        }
        if (item.state == ExtensionState::Faulted)
        {
            SendResponse(clientSocket,
                         BuildDenial(ErrorCodeExtensionFaulted,
                                     std::format("The extension {} is disabled until the extensions are reloaded (it "
                                                 "failed too often, ran out of its budget or stopped answering)",
                                                 item.extensionName)));
            return false;
        }
        if (_history.InGroup())
        {
            SendResponse(clientSocket,
                         BuildDenial(ErrorCodeEditGroupOpen,
                                     std::format("An edit group is open: end it before running {} {}, so the {}'s step "
                                                 "is not merged into it",
                                                 item.inspectorAction ? "an" : "a", noun,
                                                 item.inspectorAction ? "button" : "item")));
            return false;
        }
        return true;
    }

    void EditorServer::HandleInvokeMenuItem(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const InvokeMenuItemCmd cmd = InvokeMenuItemCmd::Deserialize(reader);

        if (!PassExtensionGates(clientSocket, cmd.uiRevision))
        {
            return;
        }
        const std::optional<ExtensionHost::MenuItemRef> item = _extensions->FindMenuItem(cmd.itemId);
        if (!item.has_value())
        {
            SendError(clientSocket, "Unknown menu item: " + SanitizeForLog(cmd.itemId));
            return;
        }
        ExtensionCall call;
        call.item = &*item;
        call.command = "InvokeMenuItem";
        if (!PassExtensionCallGates(clientSocket, call))
        {
            return;
        }

        // The selection and the form's values, checked before anything runs
        json selection = json::array();
        {
            const json &given = cmd.selection;
            if (!(given.is_null() || given.is_array() || (given.is_object() && given.empty())))
            {
                SendError(clientSocket, "selection must be a JSON array of entity ids");
                return;
            }
            if (given.size() > MaxInvokeSelection)
            {
                SendError(clientSocket, std::format("selection has more than {} entries", MaxInvokeSelection));
                return;
            }
            for (const json &id : given)
            {
                if (!id.is_string() || !ExtensionHost::IsUuidText(id.get_ref<const std::string &>()))
                {
                    SendError(clientSocket, "selection must hold entity ids (UUID strings)");
                    return;
                }
                selection.push_back(id);
            }
        }
        const std::expected<json, std::string> params = CheckInvokeParams(*item, cmd.params);
        if (!params)
        {
            SendError(clientSocket, params.error());
            return;
        }

        const bool hasRoles = RequiresAccessToken();
        const ConnectionIdentity identity = CurrentIdentity();
        ServerLog::Info(std::format("audit: user={} role={} extension={}@{} item={}{}", hasRoles ? identity.user : "-",
                                 hasRoles ? std::string(TokenRoleName(identity.role)) : "-", item->extensionName, item->sha8,
                                 item->itemId, PeerSuffix()));

        const json paramsValue = *params;
        call.invoke = [this, &item, &selection, &paramsValue](const ExtensionHost::NestedHandler &handler)
        { return _extensions->Invoke(*item, selection, paramsValue, handler); };
        RunExtensionCall(clientSocket, call);
    }

    void EditorServer::HandleInvokeInspectorAction(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const InvokeInspectorActionCmd cmd = InvokeInspectorActionCmd::Deserialize(reader);

        if (!PassExtensionGates(clientSocket, cmd.uiRevision))
        {
            return;
        }
        const std::optional<ExtensionHost::MenuItemRef> action = _extensions->FindInspectorAction(cmd.actionId);
        if (!action.has_value())
        {
            SendError(clientSocket, "Unknown inspector button: " + SanitizeForLog(cmd.actionId));
            return;
        }
        ExtensionCall call;
        call.item = &*action;
        call.command = "InvokeInspectorAction";
        if (!PassExtensionCallGates(clientSocket, call))
        {
            return;
        }

        // The component: it exists in the open scene and is of the type the button's section was registered for, so a
        // button only ever runs on the component its section is shown for
        const std::expected<FoundComponent, std::string> component = FindSceneComponent(cmd.componentId);
        if (!component)
        {
            SendError(clientSocket, component.error());
            return;
        }
        if (component->typeName != action->componentType)
        {
            SendError(clientSocket, std::format("The component {} is a {}; the inspector button {} is for a {}",
                                                SanitizeForLog(cmd.componentId), SanitizeForLog(component->typeName),
                                                action->itemId, action->componentType));
            return;
        }
        const std::expected<json, std::string> params = CheckInvokeParams(*action, cmd.params);
        if (!params)
        {
            SendError(clientSocket, params.error());
            return;
        }
        // The UUID as the scene writes it (one spelling), not as the client sent it
        const std::string componentId = Math::UUID::FromString(cmd.componentId)->ToString();

        const bool hasRoles = RequiresAccessToken();
        const ConnectionIdentity identity = CurrentIdentity();
        ServerLog::Info(std::format("audit: user={} role={} extension={}@{} action={} component={}{}",
                                 hasRoles ? identity.user : "-", hasRoles ? std::string(TokenRoleName(identity.role)) : "-",
                                 action->extensionName, action->sha8, action->itemId, componentId, PeerSuffix()));

        const json paramsValue = *params;
        const std::string entityId = component->entityId;
        call.invoke = [this, &action, &componentId, &entityId, &paramsValue](const ExtensionHost::NestedHandler &handler)
        { return _extensions->InvokeAction(*action, componentId, entityId, paramsValue, handler); };
        RunExtensionCall(clientSocket, call);
    }

    void EditorServer::RunExtensionCall(int clientSocket, const ExtensionCall &call)
    {
        const ExtensionHost::MenuItemRef &item = *call.item;
        const std::string noun = NounOf(item);

        // One invocation is one undo step, labelled with the item's title (a button's label)
        std::string label = SanitizeForLog(item.title, 100);
        if (label.empty())
        {
            label = "Edit";
        }
        std::string via = item.extensionName;
        std::ranges::replace(via, ' ', '_');

        ExtensionInvocation invocation;
        invocation.via = via;
        invocation.name = item.extensionName;
        invocation.capabilities = item.capabilities;

        const uint32_t revisionBefore = _sceneRevision;
        const auto started = std::chrono::steady_clock::now();
        if (!_history.BeginGroup(label))
        {
            SendError(clientSocket, "The edit history could not open a group for the call");
            return;
        }

        ExtensionHost::InvokeOutcome outcome;
        {
            // Set for the call and cleared on every way out of it
            struct InvocationScope
            {
                ExtensionInvocation *&slot;
                InvocationScope(ExtensionInvocation *&where, ExtensionInvocation *value) : slot(where) { slot = value; }
                ~InvocationScope() { slot = nullptr; }
                InvocationScope(const InvocationScope &) = delete;
                InvocationScope &operator=(const InvocationScope &) = delete;
            } scope(_invocation, &invocation);

            try
            {
                outcome = call.invoke(
                    [this](const std::string &op, const json &args) -> ExtensionHost::NestedAnswer
                    {
                        ExtensionHost::NestedAnswer answer;
                        std::string error;
                        answer.value = ExtensionRequest(op, args, error);
                        if (!error.empty())
                        {
                            answer.ok = false;
                            answer.error = std::move(error);
                        }
                        return answer;
                    });
            }
            catch (const std::exception &e)
            {
                outcome = ExtensionHost::InvokeOutcome{};
                outcome.failure = ExtensionHost::InvokeFailure::HostError;
                outcome.error = SanitizeForLog(e.what(), 300);
            }
            catch (...)
            {
                outcome = ExtensionHost::InvokeOutcome{};
                outcome.failure = ExtensionHost::InvokeFailure::HostError;
                outcome.error = "the call failed";
            }
        }
        if (outcome.Ok() && !invocation.fatal.empty())
        {
            // A call that sent several commands could not be taken back alone: the whole invocation is, whatever the
            // script made of the error (not counted against the extension: the host could not do it)
            outcome = ExtensionHost::InvokeOutcome{};
            outcome.failure = ExtensionHost::InvokeFailure::HostError;
            outcome.error = invocation.fatal;
        }
        const auto elapsedMillis =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

        if (outcome.Ok())
        {
            // The group ends as EndEditGroup ends one: a step, no step for writes that left the scene as it was, or none
            const EditHistory::GroupEnd ended = _history.EndGroup();
            // An empty group whose writes moved the revision (a multi-command call taken back, all the script changed)
            // left the scene as it was: as for NoChange, it is as saved as it was
            const bool restored = ended == EditHistory::GroupEnd::NoChange ||
                                  (ended == EditHistory::GroupEnd::Empty && _sceneRevision != revisionBefore);
            if (ended == EditHistory::GroupEnd::Committed || restored)
            {
                PushHistoryChanged();
            }
            if (ended == EditHistory::GroupEnd::Committed)
            {
                NoteEditStepEnded();
            }
            else if (restored)
            {
                NoteGroupWithoutChange();
            }
            ServerLog::Info(std::format("Editor extension {}@{} {} {} finished in {} ms: {} command(s){}",
                                     item.extensionName, item.sha8, item.inspectorAction ? "action" : "item", item.itemId,
                                     elapsedMillis, outcome.nestedCommands,
                                     ended == EditHistory::GroupEnd::Committed ? ", one undo step" : ", no undo step"));

            BufferWriter response;
            WriteExtensionResult(response, label, _sceneRevision, _history.CanUndo(), _history.CanRedo(), _savedRevision,
                                 outcome.result);
            SendResponse(clientSocket, response.Release());
            return;
        }

        // A failure is atomic: what the call did is taken back, with no step in the history and the redo steps kept
        std::string failureText = outcome.error;
        const std::expected<EditEffect, std::string> aborted = _history.AbortGroup();
        if (!aborted)
        {
            // An op could not be undone: the history was cleared, and a client refetches everything
            MarkSceneChanged({}, true);
            PushHistoryChanged();
            failureText += " (" + aborted.error() + ")";
        }
        else if (_sceneRevision != revisionBefore)
        {
            // The writes moved the revision and the take-back is a change of its own: the scene is what it was, so it is as
            // saved as it was (a scene that was not at its saved state is still not)
            ++_sceneRevision;
            NoteSceneChanged();
            if (aborted->inexact)
            {
                // As after an undo: an object cannot lose the transform a write gave it, so the scene is not exactly
                // what was saved even when the history says it is
                _history.ForgetSaved();
            }
            if (_history.IsAtSavedState())
            {
                _savedRevision = _sceneRevision;
            }
            EditEffect effect = *aborted;
            PushSceneChanged(std::move(effect.entityIds), effect.full);
            NoteEditStepEnded();
        }

        // The detail goes to the Admin log; the client's message has no host path (the extension host stripped them, and
        // ExecuteCommand redacts a remote client's frame as well)
        ServerLog::Warn(std::format("Editor extension {}@{} {} {} failed after {} ms and was taken back{}: {}",
                                 item.extensionName, item.sha8, item.inspectorAction ? "action" : "item", item.itemId,
                                 elapsedMillis, outcome.faulted ? " (the extension is now disabled until reload)" : "",
                                 failureText));
        SendResponse(clientSocket,
                     BuildDenial(ErrorCodeExtensionFailed,
                                 std::format("The {} {} of the extension {} failed and its changes were taken "
                                             "back: {}{}",
                                             noun, item.itemId, item.extensionName, failureText,
                                             outcome.faulted ? " (the extension is now disabled until reload)" : "")));
    }
}
