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
#include <stdexcept>
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

    /// The deepest nesting, and the most values, a json field a client sends may have where the host keeps the value
    /// (a component's fields, the project's settings): deeper text is refused before it is parsed, since a tree
    /// 100 000 deep would overflow the stack of every recursive walk that copies, saves or dumps it.
    inline constexpr std::size_t MaxJsonDepth = 64;
    inline constexpr std::size_t MaxJsonValues = 200000;

    /// Throws std::runtime_error when the JSON text nests deeper than maxDepth (counting [ and { outside strings)
    inline void CheckJsonDepth(std::string_view text, const std::size_t maxDepth)
    {
        std::size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (const char c : text)
        {
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '[' || c == '{')
            {
                if (++depth > maxDepth)
                    throw std::runtime_error("the JSON nests more than " + std::to_string(maxDepth) + " levels deep");
            }
            else if ((c == ']' || c == '}') && depth > 0)
                --depth;
        }
    }

    /// ReadJson with the limits above: nesting checked before parsing, the number of values after. Throws
    /// std::runtime_error (an Error response) for either, and nlohmann::json::parse_error for text that isn't JSON.
    inline nlohmann::json ReadBoundedJson(BufferReader &r)
    {
        const std::string text = r.ReadString();
        CheckJsonDepth(text, MaxJsonDepth);
        nlohmann::json value = nlohmann::json::parse(text);

        std::size_t count = 0;
        std::vector<const nlohmann::json *> pending{&value};
        while (!pending.empty())
        {
            const nlohmann::json *item = pending.back();
            pending.pop_back();
            if (++count > MaxJsonValues)
                throw std::runtime_error("the JSON has more than " + std::to_string(MaxJsonValues) + " values");
            if (item->is_structured())
            {
                for (const nlohmann::json &child : *item)
                    pending.push_back(&child);
            }
        }
        return value;
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
        /// The epoch afterSeq belongs to (the previous poll's epoch), or 0 when the client doesn't know it
        uint32_t epoch;
        /// The last seq the client has seen (the previous poll's nextSeq), or 0 for everything the server still has
        uint32_t afterSeq;
        /// The most events to return (the server caps it; 0 returns none and skips to the newest)
        uint32_t maxEvents;

        static PollEventsCmd Deserialize(BufferReader &r)
        {
            return {r.ReadU32(), r.ReadU32(), r.ReadU32()};
        }
    };

    struct RenderFrameIfChangedCmd
    {
        /// The revision of the frame the client holds, or 0 for none
        uint32_t sinceRevision;

        static RenderFrameIfChangedCmd Deserialize(BufferReader &r)
        {
            return {r.ReadU32()};
        }
    };

    struct SetEditorCameraCmd
    {
        Math::Vector3 position;
        Math::Quaternion rotation;
        float fovY;
        bool orthographic;
        float orthoSize;
        float nearPlane;
        float farPlane;

        static SetEditorCameraCmd Deserialize(BufferReader &r)
        {
            // Read in order, one statement each (the arguments of a call have no defined order)
            SetEditorCameraCmd cmd;
            cmd.position = ReadVec3(r);
            cmd.rotation = ReadQuat(r);
            cmd.fovY = r.ReadF32();
            cmd.orthographic = r.ReadBool();
            cmd.orthoSize = r.ReadF32();
            cmd.nearPlane = r.ReadF32();
            cmd.farPlane = r.ReadF32();
            return cmd;
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

    struct OpenSceneCmd
    {
        /// The scene file: a res:// path ending in .scene
        std::string path;

        static OpenSceneCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct SaveSceneToFileCmd
    {
        /// Where to save: a res:// path ending in .scene, or empty for the open scene's own file
        std::string path;

        static SaveSceneToFileCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct NewSceneCmd
    {
        /// The new scene's file (a res:// path ending in .scene, which mustn't exist yet), or empty for a scene with
        /// no file until it is saved
        std::string path;
        /// The scene's name; empty: the file's name without its extension ("Untitled" without a path)
        std::string name;

        static NewSceneCmd Deserialize(BufferReader &r)
        {
            // Braced initialisation evaluates left to right, so the fields are read in order
            return {r.ReadString(), r.ReadString()};
        }
    };

    struct SetProjectSettingsCmd
    {
        /// A JSON merge patch (RFC 7386) for the project file's settings object
        nlohmann::json settings;

        static SetProjectSettingsCmd Deserialize(BufferReader &r)
        {
            return {ReadBoundedJson(r)};
        }
    };

    struct SetStartupSceneCmd
    {
        /// A res:// path to an existing .scene file, or empty for none
        std::string path;

        static SetStartupSceneCmd Deserialize(BufferReader &r)
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

    struct CreateEntityExCmd
    {
        /// The new object's name; empty: the preset's (or "GameObject")
        std::string name;
        /// The parent's UUID string, or empty for a root of the scene
        std::string parentId;
        /// The place among the siblings; negative (-1) or past the last: the last place
        int32_t siblingIndex;
        /// What the object starts with: "" or "Empty", "Cube", "Sphere", "Quad", "Light", "DirectionalLight",
        /// "PointLight" or "SpotLight"
        std::string preset;

        static CreateEntityExCmd Deserialize(BufferReader &r)
        {
            // Braced initialisation evaluates left to right, so the fields are read in order
            return {r.ReadString(), r.ReadString(), r.ReadI32(), r.ReadString()};
        }
    };

    struct SetEntityParentCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// The new parent's UUID string, or empty for a root of the scene
        std::string parentId;
        /// The place among the new siblings; negative (-1) or past the last: the last place
        int32_t siblingIndex;
        /// Whether the object keeps its world transform (its local one changes) rather than its local transform
        bool keepWorldTransform;

        static SetEntityParentCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string parent = r.ReadString();
            const int32_t index = r.ReadI32();
            const bool keep = r.ReadBool();
            return {std::move(id), std::move(parent), index, keep};
        }
    };

    struct SetEntityPropertiesCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// An object with any of name (string), active (bool), tag (string), layer (integer 0 to 31)
        nlohmann::json properties;

        static SetEntityPropertiesCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            nlohmann::json properties = ReadJson(r);
            return {std::move(id), std::move(properties)};
        }
    };

    struct DuplicateEntityCmd
    {
        /// The GameObject's UUID string
        std::string entityId;

        static DuplicateEntityCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct GetEntityCmd
    {
        /// The GameObject's UUID string
        std::string entityId;

        static GetEntityCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct SetLocalTransformCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// Relative to its parent (to the scene for a root)
        Math::Vector3 position;
        Math::Quaternion rotation;
        Math::Vector3 scale;

        static SetLocalTransformCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            const Math::Vector3 pos = ReadVec3(r);
            const Math::Quaternion rot = ReadQuat(r);
            const Math::Vector3 scl = ReadVec3(r);
            return {std::move(id), pos, rot, scl};
        }
    };

    struct AddComponentCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// A name from GetComponentTypes
        std::string typeName;

        static AddComponentCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string type = r.ReadString();
            return {std::move(id), std::move(type)};
        }
    };

    struct RemoveComponentCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// The component's UUID string
        std::string componentId;

        static RemoveComponentCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string component = r.ReadString();
            return {std::move(id), std::move(component)};
        }
    };

    struct SetComponentFieldsCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// The component's UUID string
        std::string componentId;
        /// A partial object in the shape GetComponent returns: only its keys are set
        nlohmann::json values;

        static SetComponentFieldsCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string component = r.ReadString();
            nlohmann::json values = ReadBoundedJson(r);
            return {std::move(id), std::move(component), std::move(values)};
        }
    };

    struct GetComponentCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// The component's UUID string
        std::string componentId;

        static GetComponentCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string component = r.ReadString();
            return {std::move(id), std::move(component)};
        }
    };

    struct GetLuaFieldsCmd
    {
        /// The GameObject's UUID string
        std::string entityId;
        /// The LuaComponent's UUID string
        std::string componentId;

        static GetLuaFieldsCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            std::string component = r.ReadString();
            return {std::move(id), std::move(component)};
        }
    };

    struct BeginEditGroupCmd
    {
        /// What the group's undo step is called (an Edit menu shows it)
        std::string label;

        static BeginEditGroupCmd Deserialize(BufferReader &r)
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

    /// RenderFrameIfChanged's response: the frame revision, whether this is a frame (modified) or only the news that
    /// the client's is current, the viewport size, then the pixels (RGBA8, top row first, as FrameData's) to the end of
    /// the payload; none when modified is false
    inline void WriteFrameUpdate(BufferWriter &w, const uint32_t revision, const bool modified, const uint32_t width,
                                 const uint32_t height, std::span<const uint8_t> pixels)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::FrameUpdate));
        w.WriteU32(13 + static_cast<uint32_t>(pixels.size()));
        w.WriteU32(revision);
        w.WriteBool(modified);
        w.WriteU32(width);
        w.WriteU32(height);
        w.WriteBytes(pixels);
    }

    /// GetEditorCamera's response: the camera's fields (SetEditorCamera's) then the view and projection matrices
    inline void WriteEditorCamera(BufferWriter &w, const Math::Vector3 &position, const Math::Quaternion &rotation,
                                  const float fovY, const bool orthographic, const float orthoSize,
                                  const float nearPlane, const float farPlane, const Math::Matrix<float, 4, 4> &view,
                                  const Math::Matrix<float, 4, 4> &projection)
    {
        BufferWriter payload;
        WriteVec3(payload, position);
        WriteQuat(payload, rotation);
        payload.WriteF32(fovY);
        payload.WriteBool(orthographic);
        payload.WriteF32(orthoSize);
        payload.WriteF32(nearPlane);
        payload.WriteF32(farPlane);
        WriteMat4(payload, view);
        WriteMat4(payload, projection);

        w.WriteU8(static_cast<uint8_t>(ResponseType::EditorCamera));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
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

    /// PollEvents' response: epoch, nextSeq, dropped, then the events as one JSON array of EditorEvent objects
    inline void WriteEvents(BufferWriter &w, uint32_t epoch, uint32_t nextSeq, uint32_t dropped,
                            const nlohmann::json &events)
    {
        BufferWriter payload;
        payload.WriteU32(epoch);
        payload.WriteU32(nextSeq);
        payload.WriteU32(dropped);
        WriteJson(payload, events);

        w.WriteU8(static_cast<uint8_t>(ResponseType::Events));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// OpenScene's, NewScene's, SaveSceneToFile's and GetOpenScene's response: the open scene's file (a res:// path,
    /// or empty when it has none), its name and UUID, the scene revision and the revision last saved (or opened)
    inline void WriteSceneInfo(BufferWriter &w, const std::string &path, const std::string &name,
                               const std::string &uuid, uint32_t revision, uint32_t savedRevision)
    {
        BufferWriter payload;
        payload.WriteString(path);
        payload.WriteString(name);
        payload.WriteString(uuid);
        payload.WriteU32(revision);
        payload.WriteU32(savedRevision);

        w.WriteU8(static_cast<uint8_t>(ResponseType::SceneInfo));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetProjectInfo's, SetProjectSettings' and SetStartupScene's response: the project's folder and its user://
    /// folder (both absolute, UTF-8), and its project.n2proj as JSON
    inline void WriteProjectInfo(BufferWriter &w, const std::string &rootPath, const std::string &userDataPath,
                                 const nlohmann::json &project)
    {
        BufferWriter payload;
        payload.WriteString(rootPath);
        payload.WriteString(userDataPath);
        WriteJson(payload, project);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ProjectInfo));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetHierarchy's response: the scene revision the nodes were read at, then the nodes as one JSON array of
    /// HierarchyNode objects
    inline void WriteHierarchy(BufferWriter &w, uint32_t revision, const nlohmann::json &nodes)
    {
        BufferWriter payload;
        payload.WriteU32(revision);
        WriteJson(payload, nodes);

        w.WriteU8(static_cast<uint8_t>(ResponseType::Hierarchy));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetEntity's response: the entity as one JSON object (EntityDetails), then its world matrix
    inline void WriteEntityData(BufferWriter &w, const nlohmann::json &entity,
                                const Math::Matrix<float, 4, 4> &worldMatrix)
    {
        BufferWriter payload;
        WriteJson(payload, entity);
        WriteMat4(payload, worldMatrix);

        w.WriteU8(static_cast<uint8_t>(ResponseType::EntityData));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetComponentTypes' response: the types as one JSON array of ComponentSchema objects
    inline void WriteComponentTypes(BufferWriter &w, const nlohmann::json &types)
    {
        BufferWriter payload;
        WriteJson(payload, types);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ComponentTypes));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// AddComponent's response: the new component's UUID string, then its saved values as JSON
    inline void WriteComponentAdded(BufferWriter &w, const std::string &componentId, const nlohmann::json &values)
    {
        BufferWriter payload;
        payload.WriteString(componentId);
        WriteJson(payload, values);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ComponentAdded));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// SetComponentFields' and GetComponent's response: the component's saved values as JSON
    inline void WriteComponentData(BufferWriter &w, const nlohmann::json &values)
    {
        BufferWriter payload;
        WriteJson(payload, values);

        w.WriteU8(static_cast<uint8_t>(ResponseType::ComponentData));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetLuaFields' response: a ComponentSchema as JSON
    inline void WriteLuaFields(BufferWriter &w, const nlohmann::json &schema)
    {
        BufferWriter payload;
        WriteJson(payload, schema);

        w.WriteU8(static_cast<uint8_t>(ResponseType::LuaFields));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// Undo's and Redo's response: the label of the step undone or redone, the scene revision after it, whether
    /// another step can be undone and redone, and the revision last saved (or opened)
    inline void WriteEditResult(BufferWriter &w, const std::string &label, uint32_t revision, bool canUndo,
                                bool canRedo, uint32_t savedRevision)
    {
        BufferWriter payload;
        payload.WriteString(label);
        payload.WriteU32(revision);
        payload.WriteBool(canUndo);
        payload.WriteBool(canRedo);
        payload.WriteU32(savedRevision);

        w.WriteU8(static_cast<uint8_t>(ResponseType::EditResult));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetHistory's response: how many steps are done, then the steps (oldest first) as one JSON array of
    /// HistoryEntry objects
    inline void WriteHistory(BufferWriter &w, uint32_t cursor, const nlohmann::json &entries)
    {
        BufferWriter payload;
        payload.WriteU32(cursor);
        WriteJson(payload, entries);

        w.WriteU8(static_cast<uint8_t>(ResponseType::History));
        w.WriteU32(static_cast<uint32_t>(payload.Size()));
        w.WriteBytes(payload.Data());
    }

    /// GetAutosave's response: an AutosaveInfo as JSON
    inline void WriteAutosave(BufferWriter &w, const nlohmann::json &info)
    {
        BufferWriter payload;
        WriteJson(payload, info);

        w.WriteU8(static_cast<uint8_t>(ResponseType::Autosave));
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