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
inline constexpr const char *ProtocolVersion = "1.5.0";

enum class CommandType : uint8_t
{
    RenderFrame = 0x01,
    SetViewportSize = 0x02,
    GetAudio = 0x03,
    Hello = 0x04,
    PollEvents = 0x05,
    SetCameraPosition = 0x10,
    GetCameraPosition = 0x12,
    CreateScene = 0x20,
    LoadScene = 0x21,
    SaveScene = 0x22,
    DeleteScene = 0x23,
    GetCurrentScene = 0x24,
    OpenScene = 0x25,
    SaveSceneToFile = 0x26,
    NewScene = 0x27,
    GetHierarchy = 0x28,
    GetOpenScene = 0x29,
    CreateEntity = 0x30,
    DestroyEntity = 0x31,
    SetEntityTransform = 0x32,
    GetEntityTransform = 0x33,
    GetAllEntities = 0x34,
    CreateEntityEx = 0x35,
    SetEntityParent = 0x36,
    SetEntityProperties = 0x37,
    DuplicateEntity = 0x38,
    GetEntity = 0x39,
    SetLocalTransform = 0x3A,
    CreateScript = 0x40,
    RescanAssets = 0x41,
    GetEngineHealth = 0x50,
    GetComponentTypes = 0x60,
    AddComponent = 0x61,
    RemoveComponent = 0x62,
    SetComponentFields = 0x63,
    GetComponent = 0x64,
    GetLuaFields = 0x65,
    GetProjectInfo = 0x70,
    SetProjectSettings = 0x71,
    SetStartupScene = 0x72,
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
    Events = 0x0C,
    SceneInfo = 0x0D,
    ProjectInfo = 0x0E,
    Hierarchy = 0x0F,
    EntityData = 0x10,
    ComponentTypes = 0x11,
    ComponentAdded = 0x12,
    ComponentData = 0x13,
    LuaFields = 0x14,
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

struct PollEventsCmd
{
    uint32_t epoch;
    uint32_t afterSeq;
    uint32_t maxEvents;
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

struct OpenSceneCmd
{
    std::string path;
};

struct SaveSceneToFileCmd
{
    std::string path;
};

struct NewSceneCmd
{
    std::string path;
    std::string name;
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

struct CreateEntityExCmd
{
    std::string name;
    std::string parentId;
    int32_t siblingIndex;
    std::string preset;
};

struct SetEntityParentCmd
{
    std::string entityId;
    std::string parentId;
    int32_t siblingIndex;
    bool keepWorldTransform;
};

struct SetEntityPropertiesCmd
{
    std::string entityId;
    std::string properties; // JSON: any
};

struct DuplicateEntityCmd
{
    std::string entityId;
};

struct GetEntityCmd
{
    std::string entityId;
};

struct SetLocalTransformCmd
{
    std::string entityId;
    Vec3 position;
    Quat rotation;
    Vec3 scale;
};

struct CreateScriptCmd
{
    std::string name;
};

struct AddComponentCmd
{
    std::string entityId;
    std::string typeName;
};

struct RemoveComponentCmd
{
    std::string entityId;
    std::string componentId;
};

struct SetComponentFieldsCmd
{
    std::string entityId;
    std::string componentId;
    std::string values; // JSON: any
};

struct GetComponentCmd
{
    std::string entityId;
    std::string componentId;
};

struct GetLuaFieldsCmd
{
    std::string entityId;
    std::string componentId;
};

struct SetProjectSettingsCmd
{
    std::string settings; // JSON: any
};

struct SetStartupSceneCmd
{
    std::string path;
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

struct EventsData
{
    uint32_t epoch;
    uint32_t nextSeq;
    uint32_t dropped;
    std::string events; // JSON: EditorEvent[]
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

struct SceneInfoData
{
    std::string path;
    std::string name;
    std::string uuid;
    uint32_t revision;
    uint32_t savedRevision;
};

struct HierarchyData
{
    uint32_t revision;
    std::string nodes; // JSON: HierarchyNode[]
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

struct EntityDataData
{
    std::string entity; // JSON: EntityDetails
    std::array<float, 16> worldMatrix;
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

struct ComponentTypesData
{
    std::string types; // JSON: ComponentSchema[]
};

struct ComponentAddedData
{
    std::string componentId;
    std::string values; // JSON: any
};

struct ComponentDataData
{
    std::string values; // JSON: any
};

struct LuaFieldsData
{
    std::string schema; // JSON: ComponentSchema
};

struct ProjectInfoData
{
    std::string rootPath;
    std::string userDataPath;
    std::string project; // JSON: ProjectFile
};

} // namespace N2Engine::Editor::Protocol
