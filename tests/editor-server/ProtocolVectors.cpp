#include "ProtocolVectors.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string_view>

#include <editor-server/Commands.hpp>
#include <editor-server/Protocol.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

namespace
{
    json Vec3Json(const Math::Vector3 &v)
    {
        return {{"x", v.x}, {"y", v.y}, {"z", v.z}};
    }

    Math::Vector3 ToVec3(const json &v)
    {
        return Math::Vector3(v.at("x").get<float>(), v.at("y").get<float>(), v.at("z").get<float>());
    }

    json QuatJson(const Math::Quaternion &q)
    {
        return {{"x", q.GetX()}, {"y", q.GetY()}, {"z", q.GetZ()}, {"w", q.GetW()}};
    }

    std::string StringField(const json &fields, const char *name)
    {
        return fields.at(name).get<std::string>();
    }

    uint32_t Uint32Field(const json &fields, const char *name)
    {
        return fields.at(name).get<uint32_t>();
    }
}

namespace ProtocolVectors
{
    json Load()
    {
        std::ifstream file(N2_PROTOCOL_TEST_VECTORS);
        if (!file)
        {
            ADD_FAILURE() << "can't open " << N2_PROTOCOL_TEST_VECTORS;
            return json::object();
        }
        return json::parse(file);
    }

    std::map<std::string, json> LoadRequests()
    {
        std::map<std::string, json> result;
        const json vectors = Load();
        for (const json &vector : vectors.value("requests", json::array()))
        {
            result[vector.at("command").get<std::string>()] = vector;
        }
        return result;
    }

    std::map<std::string, json> LoadResponses()
    {
        std::map<std::string, json> result;
        const json vectors = Load();
        for (const json &vector : vectors.value("responses", json::array()))
        {
            result[vector.at("response").get<std::string>()] = vector;
        }
        return result;
    }

    std::vector<uint8_t> FromHex(const std::string &hex)
    {
        if (hex.size() % 2 != 0)
            throw std::invalid_argument("odd-length hex");
        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        return bytes;
    }

    std::string ToHex(const std::vector<uint8_t> &bytes)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const uint8_t byte : bytes)
        {
            hex += digits[byte >> 4];
            hex += digits[byte & 0x0F];
        }
        return hex;
    }

    const std::map<std::string, RequestDecoder> &RequestDecoders()
    {
        static const std::map<std::string, RequestDecoder> decoders = {
            {"SetViewportSize", [](BufferReader &r)
            {
                const auto cmd = SetViewportSizeCmd::Deserialize(r);
                return json{{"width", cmd.width}, {"height", cmd.height}};
            }},
            {"Hello", [](BufferReader &r)
            {
                const auto cmd = HelloCmd::Deserialize(r);
                return json{{"clientName", cmd.clientName}, {"protocolVersion", cmd.protocolVersion}, {"token", cmd.token}};
            }},
            {"PollEvents", [](BufferReader &r)
            {
                const auto cmd = PollEventsCmd::Deserialize(r);
                return json{{"epoch", cmd.epoch}, {"afterSeq", cmd.afterSeq}, {"maxEvents", cmd.maxEvents}};
            }},
            {"SetCameraPosition", [](BufferReader &r)
            {
                const auto cmd = SetCameraPositionCmd::Deserialize(r);
                return json{{"x", cmd.x}, {"y", cmd.y}, {"z", cmd.z}};
            }},
            {"RenderFrameIfChanged", [](BufferReader &r)
            {
                return json{{"sinceRevision", RenderFrameIfChangedCmd::Deserialize(r).sinceRevision}};
            }},
            {"SetEditorCamera", [](BufferReader &r)
            {
                const auto cmd = SetEditorCameraCmd::Deserialize(r);
                return json{{"position", Vec3Json(cmd.position)}, {"rotation", QuatJson(cmd.rotation)},
                            {"fovY", cmd.fovY}, {"orthographic", cmd.orthographic}, {"orthoSize", cmd.orthoSize},
                            {"nearPlane", cmd.nearPlane}, {"farPlane", cmd.farPlane}};
            }},
            {"CreateScene", [](BufferReader &r) { return json{{"name", CreateSceneCmd::Deserialize(r).name}}; }},
            {"LoadScene", [](BufferReader &r) { return json{{"sceneJson", LoadSceneCmd::Deserialize(r).sceneJson}}; }},
            {"DeleteScene", [](BufferReader &r) { return json{{"sceneName", DeleteSceneCmd::Deserialize(r).sceneName}}; }},
            {"CreateEntity", [](BufferReader &r) { return json{{"name", CreateEntityCmd::Deserialize(r).name}}; }},
            {"DestroyEntity", [](BufferReader &r) { return json{{"entityId", DestroyEntityCmd::Deserialize(r).entityId}}; }},
            {"SetEntityTransform", [](BufferReader &r)
            {
                const auto cmd = SetEntityTransformCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"position", Vec3Json(cmd.position)},
                            {"rotation", Vec3Json(cmd.rotation)}, {"scale", Vec3Json(cmd.scale)}};
            }},
            {"GetEntityTransform", [](BufferReader &r)
            {
                return json{{"entityId", GetEntityTransformCmd::Deserialize(r).entityId}};
            }},
            {"AddComponent", [](BufferReader &r)
            {
                const auto cmd = AddComponentCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"typeName", cmd.typeName}};
            }},
            {"RemoveComponent", [](BufferReader &r)
            {
                const auto cmd = RemoveComponentCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"componentId", cmd.componentId}};
            }},
            {"SetComponentFields", [](BufferReader &r)
            {
                const auto cmd = SetComponentFieldsCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"componentId", cmd.componentId}, {"values", cmd.values}};
            }},
            {"GetComponent", [](BufferReader &r)
            {
                const auto cmd = GetComponentCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"componentId", cmd.componentId}};
            }},
            {"GetLuaFields", [](BufferReader &r)
            {
                const auto cmd = GetLuaFieldsCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"componentId", cmd.componentId}};
            }},
            {"BeginEditGroup", [](BufferReader &r) { return json{{"label", BeginEditGroupCmd::Deserialize(r).label}}; }},
            {"CreateEntityEx", [](BufferReader &r)
            {
                const auto cmd = CreateEntityExCmd::Deserialize(r);
                return json{{"name", cmd.name}, {"parentId", cmd.parentId}, {"siblingIndex", cmd.siblingIndex},
                            {"preset", cmd.preset}};
            }},
            {"SetEntityParent", [](BufferReader &r)
            {
                const auto cmd = SetEntityParentCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"parentId", cmd.parentId},
                            {"siblingIndex", cmd.siblingIndex}, {"keepWorldTransform", cmd.keepWorldTransform}};
            }},
            {"SetEntityProperties", [](BufferReader &r)
            {
                const auto cmd = SetEntityPropertiesCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"properties", cmd.properties}};
            }},
            {"DuplicateEntity", [](BufferReader &r) { return json{{"entityId", DuplicateEntityCmd::Deserialize(r).entityId}}; }},
            {"GetEntity", [](BufferReader &r) { return json{{"entityId", GetEntityCmd::Deserialize(r).entityId}}; }},
            {"SetLocalTransform", [](BufferReader &r)
            {
                const auto cmd = SetLocalTransformCmd::Deserialize(r);
                return json{{"entityId", cmd.entityId}, {"position", Vec3Json(cmd.position)},
                            {"rotation", QuatJson(cmd.rotation)}, {"scale", Vec3Json(cmd.scale)}};
            }},
            {"CreateScript", [](BufferReader &r) { return json{{"name", CreateScriptCmd::Deserialize(r).name}}; }},
            {"OpenScene", [](BufferReader &r) { return json{{"path", OpenSceneCmd::Deserialize(r).path}}; }},
            {"SaveSceneToFile", [](BufferReader &r) { return json{{"path", SaveSceneToFileCmd::Deserialize(r).path}}; }},
            {"NewScene", [](BufferReader &r)
            {
                const auto cmd = NewSceneCmd::Deserialize(r);
                return json{{"path", cmd.path}, {"name", cmd.name}};
            }},
            {"SetProjectSettings", [](BufferReader &r)
            {
                return json{{"settings", SetProjectSettingsCmd::Deserialize(r).settings}};
            }},
            {"SetStartupScene", [](BufferReader &r) { return json{{"path", SetStartupSceneCmd::Deserialize(r).path}}; }},
        };
        return decoders;
    }

    const std::map<std::string, ResponseBuilder> &ResponseBuilders()
    {
        static const std::map<std::string, ResponseBuilder> builders = {
            {"Ok", [](BufferWriter &w, const json &) { WriteOk(w); }},
            {"Error", [](BufferWriter &w, const json &f) { WriteError(w, StringField(f, "message")); }},
            {"FrameData", [](BufferWriter &w, const json &f)
            {
                const std::vector<uint8_t> pixels = FromHex(StringField(f, "pixels"));
                WriteFrameData(w, Uint32Field(f, "width"), Uint32Field(f, "height"), pixels);
            }},
            {"AudioSamples", [](BufferWriter &w, const json &f)
            {
                const std::vector<uint8_t> samples = FromHex(StringField(f, "samples"));
                WriteAudioSamples(w, Uint32Field(f, "sampleRate"), Uint32Field(f, "channels"),
                                  StringField(f, "sampleFormat"), Uint32Field(f, "frameCount"),
                                  Uint32Field(f, "droppedFrames"), samples);
            }},
            {"ServerInfo", [](BufferWriter &w, const json &f)
            {
                WriteServerInfo(w, StringField(f, "protocolVersion"), StringField(f, "engineVersion"),
                                f.at("capabilities"), f.at("projectLoaded").get<bool>());
            }},
            {"Events", [](BufferWriter &w, const json &f)
            {
                WriteEvents(w, Uint32Field(f, "epoch"), Uint32Field(f, "nextSeq"), Uint32Field(f, "dropped"),
                            f.at("events"));
            }},
            {"CameraPosition", [](BufferWriter &w, const json &f)
            {
                WriteCameraPosition(w, f.at("x").get<float>(), f.at("y").get<float>(), f.at("z").get<float>());
            }},
            {"SceneData", [](BufferWriter &w, const json &f) { WriteSceneData(w, StringField(f, "sceneJson")); }},
            {"EntityCreated", [](BufferWriter &w, const json &f) { WriteEntityCreated(w, StringField(f, "entityId")); }},
            {"EntityTransform", [](BufferWriter &w, const json &f)
            {
                WriteEntityTransform(w, ToVec3(f.at("position")), ToVec3(f.at("rotation")), ToVec3(f.at("scale")));
            }},
            {"EntityList", [](BufferWriter &w, const json &f)
            {
                std::vector<EntityInfo> entities;
                for (const json &entity : f.at("entities"))
                {
                    entities.push_back({StringField(entity, "id"), StringField(entity, "name")});
                }
                WriteEntityList(w, entities);
            }},
            {"ScriptData", [](BufferWriter &w, const json &f) { WriteScriptData(w, StringField(f, "scriptTemplate")); }},
            {"SceneInfo", [](BufferWriter &w, const json &f)
            {
                WriteSceneInfo(w, StringField(f, "path"), StringField(f, "name"), StringField(f, "uuid"),
                               Uint32Field(f, "revision"), Uint32Field(f, "savedRevision"));
            }},
            {"ProjectInfo", [](BufferWriter &w, const json &f)
            {
                WriteProjectInfo(w, StringField(f, "rootPath"), StringField(f, "userDataPath"), f.at("project"));
            }},
            {"Hierarchy", [](BufferWriter &w, const json &f)
            {
                WriteHierarchy(w, Uint32Field(f, "revision"), f.at("nodes"));
            }},
            {"EntityData", [](BufferWriter &w, const json &f)
            {
                // The vector's matrix is 16 numbers, column-major; Matrix is row-major (element (row, col))
                Math::Matrix<float, 4, 4> matrix;
                const json &values = f.at("worldMatrix");
                for (size_t col = 0; col < 4; ++col)
                {
                    for (size_t row = 0; row < 4; ++row)
                    {
                        matrix(row, col) = values.at(col * 4 + row).get<float>();
                    }
                }
                WriteEntityData(w, f.at("entity"), matrix);
            }},
            {"ComponentTypes", [](BufferWriter &w, const json &f) { WriteComponentTypes(w, f.at("types")); }},
            {"ComponentAdded", [](BufferWriter &w, const json &f)
            {
                WriteComponentAdded(w, StringField(f, "componentId"), f.at("values"));
            }},
            {"ComponentData", [](BufferWriter &w, const json &f) { WriteComponentData(w, f.at("values")); }},
            {"LuaFields", [](BufferWriter &w, const json &f) { WriteLuaFields(w, f.at("schema")); }},
            {"EditResult", [](BufferWriter &w, const json &f)
            {
                WriteEditResult(w, StringField(f, "label"), Uint32Field(f, "revision"), f.at("canUndo").get<bool>(),
                                f.at("canRedo").get<bool>(), Uint32Field(f, "savedRevision"));
            }},
            {"History", [](BufferWriter &w, const json &f) { WriteHistory(w, Uint32Field(f, "cursor"), f.at("entries")); }},
            {"Autosave", [](BufferWriter &w, const json &f) { WriteAutosave(w, f.at("info")); }},
            {"FrameUpdate", [](BufferWriter &w, const json &f)
            {
                const std::vector<uint8_t> pixels = FromHex(StringField(f, "pixels"));
                WriteFrameUpdate(w, Uint32Field(f, "revision"), f.at("modified").get<bool>(), Uint32Field(f, "width"),
                                 Uint32Field(f, "height"), pixels);
            }},
            {"EditorCamera", [](BufferWriter &w, const json &f)
            {
                // The vectors' matrices are 16 numbers, column-major; Matrix is row-major (element (row, col))
                const auto matrix = [&f](const char *name)
                {
                    Math::Matrix<float, 4, 4> m;
                    const json &values = f.at(name);
                    for (size_t col = 0; col < 4; ++col)
                    {
                        for (size_t row = 0; row < 4; ++row)
                        {
                            m(row, col) = values.at(col * 4 + row).get<float>();
                        }
                    }
                    return m;
                };
                const json &q = f.at("rotation");
                WriteEditorCamera(w, ToVec3(f.at("position")),
                                  Math::Quaternion(q.at("w").get<float>(), q.at("x").get<float>(),
                                                   q.at("y").get<float>(), q.at("z").get<float>()),
                                  f.at("fovY").get<float>(), f.at("orthographic").get<bool>(),
                                  f.at("orthoSize").get<float>(), f.at("nearPlane").get<float>(),
                                  f.at("farPlane").get<float>(), matrix("view"), matrix("projection"));
            }},
            {"EngineHealth", [](BufferWriter &w, const json &f)
            {
                std::vector<SubsystemStatusEntry> subsystems;
                for (const json &status : f.at("subsystems"))
                {
                    subsystems.push_back({StringField(status, "name"), StringField(status, "state"), StringField(status, "detail")});
                }
                WriteEngineHealth(w, f.at("healthy").get<bool>(), subsystems);
            }},
        };
        return builders;
    }

    namespace
    {
        json DecodeField(const nlohmann::ordered_json &spec, BufferReader &r, const std::string &type,
                         uint32_t &count)
        {
            // json first: a JSON shape can end in [] ("json:string[]") but is one string on the wire, not an array
            if (type == "json" || type.starts_with("json:"))
                return json::parse(r.ReadString());
            if (type.ends_with("[]"))
            {
                json values = json::array();
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t nestedCount = 0;
                    values.push_back(DecodeField(spec, r, type.substr(0, type.size() - 2), nestedCount));
                }
                return values;
            }
            if (type == "string")
                return r.ReadString();
            if (type == "uint8")
                return r.ReadU8();
            if (type == "bool")
                return r.ReadBool();
            if (type == "uint32")
                return r.ReadU32();
            if (type == "int32")
                return r.ReadI32();
            if (type == "float32")
                return r.ReadF32();
            if (type == "mat4")
            {
                json values = json::array();
                for (int i = 0; i < 16; ++i)
                    values.push_back(r.ReadF32());
                return values;
            }
            if (type == "bytes")
            {
                const auto rest = r.ReadBytes(r.Remaining());
                return ToHex({rest.begin(), rest.end()});
            }
            json value = json::object();
            for (const auto &[field, fieldType] : spec.at("types").at(type).items())
            {
                value[field] = DecodeField(spec, r, fieldType.get<std::string>(), count);
            }
            return value;
        }
    }

    json DecodeBySpec(const nlohmann::ordered_json &spec, const nlohmann::ordered_json &fields,
                      std::span<const uint8_t> payload)
    {
        BufferReader r(payload);
        json values = json::object();
        uint32_t count = 0;
        for (const auto &[field, type] : fields.items())
        {
            values[field] = DecodeField(spec, r, type.get<std::string>(), count);
            if (field == "count")
                count = values[field].get<uint32_t>();
        }
        return values;
    }

    bool HasJsonField(const nlohmann::ordered_json &fields)
    {
        for (const auto &[field, type] : fields.items())
        {
            if (type.get<std::string>().starts_with("json"))
                return true;
        }
        return false;
    }

    std::vector<uint8_t> BuildResponse(const std::string &response, const json &vector)
    {
        const auto builder = ResponseBuilders().find(response);
        if (builder == ResponseBuilders().end())
        {
            ADD_FAILURE() << "no server builder for " << response;
            return {};
        }
        BufferWriter w;
        builder->second(w, vector.at("fields"));
        return w.Release();
    }
}
