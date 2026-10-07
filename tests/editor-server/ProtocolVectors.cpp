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
            {"SetCameraPosition", [](BufferReader &r)
            {
                const auto cmd = SetCameraPositionCmd::Deserialize(r);
                return json{{"x", cmd.x}, {"y", cmd.y}, {"z", cmd.z}};
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
            {"CreateScript", [](BufferReader &r) { return json{{"name", CreateScriptCmd::Deserialize(r).name}}; }},
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
