#pragma once

#include "Protocol.hpp"
#include "Serialization.hpp"
#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <engine/Health.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace N2Engine::Editor::Protocol
{
    // ==================== Field Codecs ====================
    // protocol.json's field types beyond BufferWriter/BufferReader's primitives (see its "encoding" section)

    inline void WriteVec3(BufferWriter &w, const Math::Vector3 &v)
    {
        w.WriteF32(v.x);
        w.WriteF32(v.y);
        w.WriteF32(v.z);
    }

    inline Math::Vector3 ReadVec3(BufferReader &r)
    {
        const float x = r.ReadF32();
        const float y = r.ReadF32();
        const float z = r.ReadF32();
        return Math::Vector3(x, y, z);
    }

    /// quat: x, y, z, w (the engine stores w first; the wire puts it last, as glTF and gl-matrix do)
    inline void WriteQuat(BufferWriter &w, const Math::Quaternion &q)
    {
        w.WriteF32(q.GetX());
        w.WriteF32(q.GetY());
        w.WriteF32(q.GetZ());
        w.WriteF32(q.GetW());
    }

    inline Math::Quaternion ReadQuat(BufferReader &r)
    {
        const float x = r.ReadF32();
        const float y = r.ReadF32();
        const float z = r.ReadF32();
        const float w = r.ReadF32();
        return Math::Quaternion(w, x, y, z);
    }

    /// mat4: 16 floats, column-major (element col * 4 + row). Matrix stores rows (Data() is row-major), so this writes
    /// its transpose; with column vectors, the translation (column 3) is elements 12, 13 and 14 on the wire.
    inline void WriteMat4(BufferWriter &w, const Math::Matrix<float, 4, 4> &m)
    {
        for (std::size_t col = 0; col < 4; ++col)
        {
            for (std::size_t row = 0; row < 4; ++row)
            {
                w.WriteF32(m(row, col));
            }
        }
    }

    inline Math::Matrix<float, 4, 4> ReadMat4(BufferReader &r)
    {
        Math::Matrix<float, 4, 4> m;
        for (std::size_t col = 0; col < 4; ++col)
        {
            for (std::size_t row = 0; row < 4; ++row)
            {
                m(row, col) = r.ReadF32();
            }
        }
        return m;
    }

    /// json: a string holding the value's compact JSON text (keys sorted, as nlohmann::json keeps them). Invalid
    /// UTF-8 in a string value is replaced (U+FFFD) rather than throwing.
    inline void WriteJson(BufferWriter &w, const nlohmann::json &value)
    {
        w.WriteString(value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    /// Throws nlohmann::json::parse_error for text that isn't JSON (ExecuteCommand turns it into an Error response)
    inline nlohmann::json ReadJson(BufferReader &r)
    {
        return nlohmann::json::parse(r.ReadString());
    }

    /// A trailing request field added in a later minor version (protocol.json's encoding.versioning): a client of an
    /// older minor version doesn't send it, so it is read only when the payload has bytes left, and is fallback
    /// otherwise. Every request field added to an existing command must be read this way. For example:
    ///     cmd.keepWorldTransform = ReadTrailing(r, &BufferReader::ReadBool, true);
    template <typename T, typename Read>
    T ReadTrailing(BufferReader &r, Read read, T fallback)
    {
        if (!r.HasData())
            return fallback;
        return static_cast<T>(std::invoke(read, r));
    }

    // ==================== Command Deserializers ====================
    // One per command with a request in protocol.json, named <Command>Cmd with its fields, like the
    // generated clients/cpp/Protocol.generated.hpp (which only declares the structs)

    struct SetViewportSizeCmd
    {
        int32_t width;
        int32_t height;

        static SetViewportSizeCmd Deserialize(BufferReader &r)
        {
            return {r.ReadI32(), r.ReadI32()};
        }
    };

    struct HelloCmd
    {
        /// For logs only
        std::string clientName;
        /// The protocol version the client speaks: "major.minor.patch"
        std::string protocolVersion;
        /// The host's access token, or empty
        std::string token;

        static HelloCmd Deserialize(BufferReader &r)
        {
            // Braced initialisation evaluates left to right, so the fields are read in order
            return {r.ReadString(), r.ReadString(), r.ReadString()};
        }
    };

    struct PollEventsCmd
    {
        /// The last seq the client has seen (the previous poll's nextSeq), or 0 for everything the server still has
        uint32_t afterSeq;
        /// The most events to return (the server caps it; 0 returns none and skips to the newest)
        uint32_t maxEvents;

        static PollEventsCmd Deserialize(BufferReader &r)
        {
            return {r.ReadU32(), r.ReadU32()};
        }
    };

    struct SetCameraPositionCmd
    {
        float x, y, z;

        static SetCameraPositionCmd Deserialize(BufferReader &r)
        {
            return {r.ReadF32(), r.ReadF32(), r.ReadF32()};
        }
    };

    struct CreateSceneCmd
    {
        std::string name;

        static CreateSceneCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct LoadSceneCmd
    {
        /// The scene's JSON text (not a path)
        std::string sceneJson;

        static LoadSceneCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct DeleteSceneCmd
    {
        /// Scene file relative to the scenes directory (see EditorServer::ResolveSceneFile), not a path
        std::string sceneName;

        static DeleteSceneCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct CreateEntityCmd
    {
        std::string name;

        static CreateEntityCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct DestroyEntityCmd
    {
        /// The GameObject's UUID string
        std::string entityId;

        static DestroyEntityCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct GetEntityTransformCmd
    {
        /// The GameObject's UUID string
        std::string entityId;

        static GetEntityTransformCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct CreateScriptCmd
    {
        std::string name;

        static CreateScriptCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct SetEntityTransformCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        Math::Vector3 position;
        Math::Vector3 rotation; // Euler angles
        Math::Vector3 scale;

        static SetEntityTransformCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            const Math::Vector3 pos = ReadVec3(r);
            const Math::Vector3 rot = ReadVec3(r);
            const Math::Vector3 scl = ReadVec3(r);
            return {std::move(id), pos, rot, scl};
        }
    };

    // ==================== Response Builders ====================

    inline void WriteOk(BufferWriter &w)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::Ok));
        w.WriteU32(0);
    }

    /// Error's payload is the message's raw bytes, not a length-prefixed string: the documented special case, kept for
    /// compatibility (protocol.json's encoding.errorPayload)
    inline void WriteError(BufferWriter &w, const std::string &msg = "")
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::Error));
        w.WriteU32(static_cast<uint32_t>(msg.size()));
        for (char c : msg) w.WriteU8(static_cast<uint8_t>(c));
    }

    inline void WriteCameraPosition(BufferWriter &w, float x, float y, float z)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::CameraPosition));
        w.WriteU32(12);
        w.WriteF32(x);
        w.WriteF32(y);
        w.WriteF32(z);
    }

    inline void WriteEntityCreated(BufferWriter &w, std::string entityId)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::EntityCreated));
        w.WriteU32(static_cast<uint32_t>(4 + entityId.size()));
        w.WriteString(entityId);
    }

    inline void WriteFrameData(BufferWriter &w, uint32_t width, uint32_t height, std::span<const uint8_t> pixels)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::FrameData));
        w.WriteU32(8 + static_cast<uint32_t>(pixels.size()));
        w.WriteU32(width);
        w.WriteU32(height);
        w.WriteBytes(pixels);
    }

    /// GetAudio's response: sampleRate, channels, sampleFormat ("float32" or "int16"), frameCount, droppedFrames,
    /// then the samples as raw bytes to the end of the payload (like FrameData's pixels): frameCount * channels
    /// interleaved little-endian samples
    inline void WriteAudioSamples(BufferWriter &w, uint32_t sampleRate, uint32_t channels,
                                  const std::string &sampleFormat, uint32_t frameCount, uint32_t droppedFrames,
                                  std::span<const uint8_t> samples)
    {
        BufferWriter header;
        header.WriteU32(sampleRate);
        header.WriteU32(channels);
        header.WriteString(sampleFormat);
        header.WriteU32(frameCount);
        header.WriteU32(droppedFrames);

        w.WriteU8(static_cast<uint8_t>(ResponseType::AudioSamples));
        w.WriteU32(static_cast<uint32_t>(header.Size() + samples.size()));
        w.WriteBytes(header.Data());
        w.WriteBytes(samples);
    }

    inline void WriteSceneData(BufferWriter &w, const std::string &jsonString)
    {
        BufferWriter payload;
        payload.WriteString(jsonString);

        w.WriteU8(static_cast<uint8_t>(ResponseType::SceneData));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// One SubsystemStatus as it goes on the wire (the state by name)
    struct SubsystemStatusEntry
    {
        std::string name;
        std::string state;
        std::string detail;
    };

    /// healthy (bool), count (u32), then per subsystem: name, state ("Running", "Failed", ...), detail
    inline void WriteEngineHealth(BufferWriter &w, bool healthy, std::span<const SubsystemStatusEntry> subsystems)
    {
        BufferWriter payload;
        payload.WriteBool(healthy);
        payload.WriteU32(static_cast<uint32_t>(subsystems.size()));
        for (const SubsystemStatusEntry &status : subsystems)
        {
            payload.WriteString(status.name);
            payload.WriteString(status.state);
            payload.WriteString(status.detail);
        }

        w.WriteU8(static_cast<uint8_t>(ResponseType::EngineHealth));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    inline void WriteEngineHealth(BufferWriter &w, const N2Engine::EngineHealth &health)
    {
        std::vector<SubsystemStatusEntry> subsystems;
        subsystems.reserve(health.subsystems.size());
        for (const N2Engine::SubsystemStatus &status : health.subsystems)
        {
            subsystems.push_back({status.name, std::string{ToString(status.state)}, status.detail});
        }
        WriteEngineHealth(w, health.IsHealthy(), subsystems);
    }

    /// GetEntityTransform's response: world position, rotation (Euler angles) and scale
    inline void WriteEntityTransform(BufferWriter &w, const Math::Vector3 &position, const Math::Vector3 &rotation,
                                     const Math::Vector3 &scale)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::EntityTransform));
        w.WriteU32(36); // 9 floats
        WriteVec3(w, position);
        WriteVec3(w, rotation);
        WriteVec3(w, scale);
    }

    /// One GameObject in GetAllEntities' list
    struct EntityInfo
    {
        /// The GameObject's UUID string
        std::string id;
        std::string name;
    };

    /// count (u32), then each entity's id and name
    inline void WriteEntityList(BufferWriter &w, std::span<const EntityInfo> entities)
    {
        BufferWriter payload;
        payload.WriteU32(static_cast<uint32_t>(entities.size()));
        for (const EntityInfo &entity : entities)
        {
            payload.WriteString(entity.id);
            payload.WriteString(entity.name);
        }

        w.WriteU8(static_cast<uint8_t>(ResponseType::EntityList));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// Hello's response: the server's protocol and engine versions, its optional features (a JSON array of names) and
    /// whether it has a project
    inline void WriteServerInfo(BufferWriter &w, std::string_view protocolVersion, std::string_view engineVersion,
                                const nlohmann::json &capabilities, bool projectLoaded)
    {
        BufferWriter payload;
        payload.WriteString(std::string{protocolVersion});
        payload.WriteString(std::string{engineVersion});
        WriteJson(payload, capabilities);
        payload.WriteBool(projectLoaded);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ServerInfo));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// PollEvents' response: nextSeq, dropped, then the events as one JSON array of EditorEvent objects
    inline void WriteEvents(BufferWriter &w, uint32_t nextSeq, uint32_t dropped, const nlohmann::json &events)
    {
        BufferWriter payload;
        payload.WriteU32(nextSeq);
        payload.WriteU32(dropped);
        WriteJson(payload, events);

        w.WriteU8(static_cast<uint8_t>(ResponseType::Events));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    inline void WriteScriptData(BufferWriter &w, const std::string &scriptTemplate)
    {
        BufferWriter payload;
        payload.WriteString(scriptTemplate);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ScriptData));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }
}