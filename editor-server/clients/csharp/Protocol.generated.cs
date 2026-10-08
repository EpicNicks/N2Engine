// Auto-generated from protocol.json by generate_csharp.py - do not edit
// Declarations only, with no encoding code; nothing uses or tests this client (unsupported).
// Optional jsonTypes keys are nullable reference and value types (string?, double?), so nullable context is on.
#nullable enable
using System;

namespace N2Engine.Editor.Protocol
{
    public static class ProtocolInfo
    {
        /// <summary>protocol.json's version (major.minor.patch); Hello sends it</summary>
        public const string ProtocolVersion = "1.9.0";
    }

    public enum CommandType : byte
    {
        RenderFrame = 0x01,
        SetViewportSize = 0x02,
        GetAudio = 0x03,
        Hello = 0x04,
        PollEvents = 0x05,
        RenderFrameIfChanged = 0x06,
        SetCameraPosition = 0x10,
        GetCameraPosition = 0x12,
        SetEditorCamera = 0x13,
        GetEditorCamera = 0x14,
        PickEntity = 0x15,
        GetEntityBounds = 0x16,
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
        ListAssets = 0xA0,
        GetAssetInfo = 0xA1,
        SetImportSettings = 0xA2,
        ReadTextAsset = 0xA3,
        WriteTextAsset = 0xA4,
        CreateScriptAsset = 0xA5,
        CreateFolder = 0xA6,
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
        Undo = 0x90,
        Redo = 0x91,
        BeginEditGroup = 0x92,
        EndEditGroup = 0x93,
        GetHistory = 0x94,
        GetAutosave = 0x95,
        RestoreAutosave = 0x96,
        DiscardAutosave = 0x97,
        Shutdown = 0xFF,
    }

    public enum ResponseType : byte
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
        EditResult = 0x15,
        History = 0x16,
        Autosave = 0x17,
        FrameUpdate = 0x18,
        EditorCamera = 0x19,
        PickResult = 0x1A,
        Bounds = 0x1B,
        AssetList = 0xA0,
        AssetDetail = 0xA1,
        TextData = 0xA2,
        AssetCreated = 0xA3,
    }

    public struct Vec3
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct Quat
    {
        public float X;
        public float Y;
        public float Z;
        public float W;
    }

    public struct EntityInfo
    {
        public string Id;
        public string Name;
    }

    public struct SubsystemStatus
    {
        public string Name;
        public string State;
        public string Detail;
    }

    public class EditorEvent
    {
        [System.Text.Json.Serialization.JsonPropertyName("seq")]
        public double Seq { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("kind")]
        public string Kind { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("level")]
        public string? Level { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("message")]
        public string? Message { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("time")]
        public double? Time { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("revision")]
        public double? Revision { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("savedRevision")]
        public double? SavedRevision { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string? Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("entityIds")]
        public string[]? EntityIds { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("full")]
        public bool? Full { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("added")]
        public string[]? Added { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("removed")]
        public string[]? Removed { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("modified")]
        public string[]? Modified { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("canUndo")]
        public bool? CanUndo { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("canRedo")]
        public bool? CanRedo { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string? Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("redoLabel")]
        public string? RedoLabel { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("undoCount")]
        public double? UndoCount { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("redoCount")]
        public double? RedoCount { get; set; }
    }

    public class HistoryEntry
    {
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("bytes")]
        public double Bytes { get; set; }
    }

    public class AutosaveInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("exists")]
        public bool Exists { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string? Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("size")]
        public double? Size { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("modified")]
        public double? Modified { get; set; }
    }

    public class SubAssetInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("key")]
        public string Key { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uuid")]
        public string Uuid { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("type")]
        public string Type { get; set; }
    }

    public class AssetInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uuid")]
        public string Uuid { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("type")]
        public string Type { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("size")]
        public double Size { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("modified")]
        public double Modified { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("subAssets")]
        public SubAssetInfo[]? SubAssets { get; set; }
    }

    public class AssetDetails
    {
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uuid")]
        public string Uuid { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("type")]
        public string Type { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("size")]
        public double Size { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("modified")]
        public double Modified { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("subAssets")]
        public SubAssetInfo[]? SubAssets { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("customData")]
        public System.Text.Json.JsonElement CustomData { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("loaded")]
        public bool Loaded { get; set; }
    }

    public class ProjectFile
    {
        [System.Text.Json.Serialization.JsonPropertyName("formatVersion")]
        public double FormatVersion { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("name")]
        public string Name { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("projectId")]
        public string ProjectId { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("engineVersion")]
        public string EngineVersion { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("startupScene")]
        public string StartupScene { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("scenes")]
        public string[] Scenes { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("settings")]
        public System.Text.Json.JsonElement Settings { get; set; }
    }

    public class HierarchyNode
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("parentId")]
        public string ParentId { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("index")]
        public double Index { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("name")]
        public string Name { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("active")]
        public bool Active { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("activeInHierarchy")]
        public bool ActiveInHierarchy { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("layer")]
        public double Layer { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("tag")]
        public string Tag { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("components")]
        public string[] Components { get; set; }
    }

    public class EntityHeader
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("parentId")]
        public string ParentId { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("index")]
        public double Index { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("name")]
        public string Name { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("active")]
        public bool Active { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("activeInHierarchy")]
        public bool ActiveInHierarchy { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("layer")]
        public double Layer { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("tag")]
        public string Tag { get; set; }
    }

    public class JsonVec3
    {
        [System.Text.Json.Serialization.JsonPropertyName("x")]
        public double X { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("y")]
        public double Y { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("z")]
        public double Z { get; set; }
    }

    public class JsonQuat
    {
        [System.Text.Json.Serialization.JsonPropertyName("x")]
        public double X { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("y")]
        public double Y { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("z")]
        public double Z { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("w")]
        public double W { get; set; }
    }

    public class EntityBounds
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("min")]
        public JsonVec3 Min { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("max")]
        public JsonVec3 Max { get; set; }
    }

    public class LocalTransform
    {
        [System.Text.Json.Serialization.JsonPropertyName("position")]
        public JsonVec3 Position { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("rotation")]
        public JsonQuat Rotation { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("scale")]
        public JsonVec3 Scale { get; set; }
    }

    public class EntityComponent
    {
        [System.Text.Json.Serialization.JsonPropertyName("type")]
        public string Type { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uuid")]
        public string Uuid { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("values")]
        public System.Text.Json.JsonElement Values { get; set; }
    }

    public class EntityDetails
    {
        [System.Text.Json.Serialization.JsonPropertyName("header")]
        public EntityHeader Header { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("transform")]
        public LocalTransform? Transform { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("components")]
        public EntityComponent[] Components { get; set; }
    }

    public class FieldSchema
    {
        [System.Text.Json.Serialization.JsonPropertyName("name")]
        public string Name { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("displayName")]
        public string DisplayName { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("kind")]
        public string Kind { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("typeName")]
        public string TypeName { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("hidden")]
        public bool Hidden { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("readOnly")]
        public bool ReadOnly { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("enumOptions")]
        public string[]? EnumOptions { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("assetType")]
        public string? AssetType { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("min")]
        public double? Min { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("max")]
        public double? Max { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("tooltip")]
        public string? Tooltip { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("container")]
        public string? Container { get; set; }
    }

    public class ComponentSchema
    {
        [System.Text.Json.Serialization.JsonPropertyName("typeName")]
        public string TypeName { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("singleton")]
        public bool Singleton { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("fields")]
        public FieldSchema[] Fields { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("defaults")]
        public System.Text.Json.JsonElement? Defaults { get; set; }
    }

    public struct SetViewportSizeRequest
    {
        public int Width;
        public int Height;
    }

    public struct HelloRequest
    {
        public string ClientName;
        public string ProtocolVersion;
        public string Token;
    }

    public struct PollEventsRequest
    {
        public uint Epoch;
        public uint AfterSeq;
        public uint MaxEvents;
    }

    public struct RenderFrameIfChangedRequest
    {
        public uint SinceRevision;
    }

    public struct SetCameraPositionRequest
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct SetEditorCameraRequest
    {
        public Vec3 Position;
        public Quat Rotation;
        public float FovY;
        public bool Orthographic;
        public float OrthoSize;
        public float NearPlane;
        public float FarPlane;
    }

    public struct PickEntityRequest
    {
        public float X;
        public float Y;
        public bool IncludeInactive;
    }

    public struct GetEntityBoundsRequest
    {
        /// <summary>JSON text: string[]</summary>
        public string EntityIds;
    }

    public struct CreateSceneRequest
    {
        public string Name;
    }

    public struct LoadSceneRequest
    {
        public string SceneJson;
    }

    public struct DeleteSceneRequest
    {
        public string SceneName;
    }

    public struct OpenSceneRequest
    {
        public string Path;
    }

    public struct SaveSceneToFileRequest
    {
        public string Path;
    }

    public struct NewSceneRequest
    {
        public string Path;
        public string Name;
    }

    public struct CreateEntityRequest
    {
        public string Name;
    }

    public struct DestroyEntityRequest
    {
        public string EntityId;
    }

    public struct SetEntityTransformRequest
    {
        public string EntityId;
        public Vec3 Position;
        public Vec3 Rotation;
        public Vec3 Scale;
    }

    public struct GetEntityTransformRequest
    {
        public string EntityId;
    }

    public struct CreateEntityExRequest
    {
        public string Name;
        public string ParentId;
        public int SiblingIndex;
        public string Preset;
    }

    public struct SetEntityParentRequest
    {
        public string EntityId;
        public string ParentId;
        public int SiblingIndex;
        public bool KeepWorldTransform;
    }

    public struct SetEntityPropertiesRequest
    {
        public string EntityId;
        /// <summary>JSON text: any JSON value</summary>
        public string Properties;
    }

    public struct DuplicateEntityRequest
    {
        public string EntityId;
    }

    public struct GetEntityRequest
    {
        public string EntityId;
    }

    public struct SetLocalTransformRequest
    {
        public string EntityId;
        public Vec3 Position;
        public Quat Rotation;
        public Vec3 Scale;
    }

    public struct CreateScriptRequest
    {
        public string Name;
    }

    public struct ListAssetsRequest
    {
        public string Folder;
        public bool Recursive;
    }

    public struct GetAssetInfoRequest
    {
        public string UuidOrPath;
    }

    public struct SetImportSettingsRequest
    {
        public string Path;
        /// <summary>JSON text: any JSON value</summary>
        public string CustomData;
    }

    public struct ReadTextAssetRequest
    {
        public string Path;
    }

    public struct WriteTextAssetRequest
    {
        public string Path;
        public string Text;
    }

    public struct CreateScriptAssetRequest
    {
        public string Path;
        public string ClassName;
    }

    public struct CreateFolderRequest
    {
        public string Path;
    }

    public struct AddComponentRequest
    {
        public string EntityId;
        public string TypeName;
    }

    public struct RemoveComponentRequest
    {
        public string EntityId;
        public string ComponentId;
    }

    public struct SetComponentFieldsRequest
    {
        public string EntityId;
        public string ComponentId;
        /// <summary>JSON text: any JSON value</summary>
        public string Values;
    }

    public struct GetComponentRequest
    {
        public string EntityId;
        public string ComponentId;
    }

    public struct GetLuaFieldsRequest
    {
        public string EntityId;
        public string ComponentId;
    }

    public struct SetProjectSettingsRequest
    {
        /// <summary>JSON text: any JSON value</summary>
        public string Settings;
    }

    public struct SetStartupSceneRequest
    {
        public string Path;
    }

    public struct BeginEditGroupRequest
    {
        public string Label;
    }

    public struct FrameDataResponse
    {
        public uint Width;
        public uint Height;
        public byte[] Pixels;
    }

    public struct AudioSamplesResponse
    {
        public uint SampleRate;
        public uint Channels;
        public string SampleFormat;
        public uint FrameCount;
        public uint DroppedFrames;
        public byte[] Samples;
    }

    public struct ServerInfoResponse
    {
        public string ProtocolVersion;
        public string EngineVersion;
        /// <summary>JSON text: string[]</summary>
        public string Capabilities;
        public bool ProjectLoaded;
    }

    public struct EventsResponse
    {
        public uint Epoch;
        public uint NextSeq;
        public uint Dropped;
        /// <summary>JSON text: EditorEvent[]</summary>
        public string Events;
    }

    public struct FrameUpdateResponse
    {
        public uint Revision;
        public bool Modified;
        public uint Width;
        public uint Height;
        public byte[] Pixels;
    }

    public struct CameraPositionResponse
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct EditorCameraResponse
    {
        public Vec3 Position;
        public Quat Rotation;
        public float FovY;
        public bool Orthographic;
        public float OrthoSize;
        public float NearPlane;
        public float FarPlane;
        public float[] View;
        public float[] Projection;
    }

    public struct PickResultResponse
    {
        public string EntityId;
        public Vec3 Point;
        public float Distance;
    }

    public struct BoundsResponse
    {
        /// <summary>JSON text: EntityBounds[]</summary>
        public string Bounds;
    }

    public struct SceneDataResponse
    {
        public string SceneJson;
    }

    public struct SceneInfoResponse
    {
        public string Path;
        public string Name;
        public string Uuid;
        public uint Revision;
        public uint SavedRevision;
    }

    public struct HierarchyResponse
    {
        public uint Revision;
        /// <summary>JSON text: HierarchyNode[]</summary>
        public string Nodes;
    }

    public struct EntityCreatedResponse
    {
        public string EntityId;
    }

    public struct EntityTransformResponse
    {
        public Vec3 Position;
        public Vec3 Rotation;
        public Vec3 Scale;
    }

    public struct EntityListResponse
    {
        public uint Count;
        public EntityInfo[] Entities;
    }

    public struct EntityDataResponse
    {
        /// <summary>JSON text: EntityDetails</summary>
        public string Entity;
        public float[] WorldMatrix;
    }

    public struct ScriptDataResponse
    {
        public string ScriptTemplate;
    }

    public struct AssetListResponse
    {
        /// <summary>JSON text: string[]</summary>
        public string Folders;
        /// <summary>JSON text: AssetInfo[]</summary>
        public string Assets;
    }

    public struct AssetDetailResponse
    {
        /// <summary>JSON text: AssetDetails</summary>
        public string Info;
    }

    public struct TextDataResponse
    {
        public string Text;
    }

    public struct AssetCreatedResponse
    {
        public string Path;
        public string Uuid;
    }

    public struct EngineHealthResponse
    {
        public bool Healthy;
        public uint Count;
        public SubsystemStatus[] Subsystems;
    }

    public struct ComponentTypesResponse
    {
        /// <summary>JSON text: ComponentSchema[]</summary>
        public string Types;
    }

    public struct ComponentAddedResponse
    {
        public string ComponentId;
        /// <summary>JSON text: any JSON value</summary>
        public string Values;
    }

    public struct ComponentDataResponse
    {
        /// <summary>JSON text: any JSON value</summary>
        public string Values;
    }

    public struct LuaFieldsResponse
    {
        /// <summary>JSON text: ComponentSchema</summary>
        public string Schema;
    }

    public struct ProjectInfoResponse
    {
        public string RootPath;
        public string UserDataPath;
        /// <summary>JSON text: ProjectFile</summary>
        public string Project;
    }

    public struct EditResultResponse
    {
        public string Label;
        public uint Revision;
        public bool CanUndo;
        public bool CanRedo;
        public uint SavedRevision;
    }

    public struct HistoryResponse
    {
        public uint Cursor;
        /// <summary>JSON text: HistoryEntry[]</summary>
        public string Entries;
    }

    public struct AutosaveResponse
    {
        /// <summary>JSON text: AutosaveInfo</summary>
        public string Info;
    }

}
