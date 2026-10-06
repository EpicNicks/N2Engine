// Auto-generated from protocol.json by generate_cpp.py - do not edit
// Declarations only: the server's codecs are hand-written (editor-server/include/editor-server/Commands.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace N2Engine::Editor::Protocol
{

// protocol.json's version (major.minor.patch); Hello sends it
inline constexpr const char *ProtocolVersion = "1.1.0";

enum class CommandType : uint8_t
{
    RenderFrame = 0x01,
    SetViewportSize = 0x02,
    GetAudio = 0x03,
    Hello = 0x04,
    SetCameraPosition = 0x10,
    GetCameraPosition = 0x12,
    CreateScene = 0x20,
    LoadScene = 0x21,
    SaveScene = 0x22,
    DeleteScene = 0x23,
    GetCurrentScene = 0x24,
    CreateEntity = 0x30,
    DestroyEntity = 0x31,
    SetEntityTransform = 0x32,
    GetEntityTransform = 0x33,
    GetAllEntities = 0x34,
    CreateScript = 0x40,
    RescanAssets = 0x41,
    GetEngineHealth = 0x50,
    Shutdown = 0xFF,
};

enum class ResponseType : uint8_t
{
    Ok = 0x00,
    Error = 0x01,
    FrameData = 0x02,
    CameraPosition = 0x03,
    EntityTransform = 0x04,
    EntityList = 0x05,
    EntityCreated = 0x06,
    SceneData = 0x07,
    ScriptData = 0x08,
    EngineHealth = 0x09,
    AudioSamples = 0x0A,
    ServerInfo = 0x0B,
};

// Custom types
struct Vec3
{
    float x;
    float y;
    float z;
};

struct Quat
{
    float x;
    float y;
    float z;
    float w;
};

struct EntityInfo
{
    std::string id;
    std::string name;
};

struct SubsystemStatus
{
    std::string name;
    std::string state;
    std::string detail;
};

// Command request structures
struct SetViewportSizeCmd
{
    int32_t width;
    int32_t height;
};

struct HelloCmd
{
    std::string clientName;
    std::string protocolVersion;
    std::string token;
};

struct SetCameraPositionCmd
{
    float x;
    float y;
    float z;
};

struct CreateSceneCmd
{
    std::string name;
};

struct LoadSceneCmd
{
    std::string sceneJson;
};

struct DeleteSceneCmd
{
    std::string sceneName;
};

struct CreateEntityCmd
{
    std::string name;
};

struct DestroyEntityCmd
{
    std::string entityId;
};

struct SetEntityTransformCmd
{
    std::string entityId;
    Vec3 position;
    Vec3 rotation;
    Vec3 scale;
};

struct GetEntityTransformCmd
{
    std::string entityId;
};

struct CreateScriptCmd
{
    std::string name;
};

// Response structures
struct FrameDataData
{
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;
};

struct AudioSamplesData
{
    uint32_t sampleRate;
    uint32_t channels;
    std::string sampleFormat;
    uint32_t frameCount;
    uint32_t droppedFrames;
    std::vector<uint8_t> samples;
};

struct ServerInfoData
{
    std::string protocolVersion;
    std::string engineVersion;
    std::string capabilities; // JSON: string[]
    bool projectLoaded;
};

struct CameraPositionData
{
    float x;
    float y;
    float z;
};

struct SceneDataData
{
    std::string sceneJson;
};

struct EntityCreatedData
{
    std::string entityId;
};

struct EntityTransformData
{
    Vec3 position;
    Vec3 rotation;
    Vec3 scale;
};

struct EntityListData
{
    uint32_t count;
    std::vector<EntityInfo> entities;
};

struct ScriptDataData
{
    std::string scriptTemplate;
};

struct EngineHealthData
{
    bool healthy;
    uint32_t count;
    std::vector<SubsystemStatus> subsystems;
};

} // namespace N2Engine::Editor::Protocol
