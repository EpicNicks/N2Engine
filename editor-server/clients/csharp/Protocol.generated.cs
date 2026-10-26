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
        public const string ProtocolVersion = "1.21.0";
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
        SerializeEntities = 0x3B,
        InstantiateEntities = 0x3C,
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
        ListEditorExtensions = 0x80,
        GetEditorUi = 0x81,
        ReloadEditorExtensions = 0x84,
        InvokeMenuItem = 0x82,
        InvokeInspectorAction = 0x83,
        Undo = 0x90,
        Redo = 0x91,
        BeginEditGroup = 0x92,
        EndEditGroup = 0x93,
        GetHistory = 0x94,
        GetAutosave = 0x95,
        RestoreAutosave = 0x96,
        DiscardAutosave = 0x97,
        WritePlaySnapshot = 0xB0,
        SetPaused = 0xB1,
        Step = 0xB2,
        GetPlayState = 0xB3,
        SendInput = 0xB4,
        Shutdown = 0xFF,
        SetEditorViewOptions = 0x17,
        CheckLuaSyntax = 0xA7,
        GetAssetPreview = 0xA8,
        CreatePrefab = 0xA9,
        InstantiatePrefab = 0xAA,
        ApplyPrefab = 0xAB,
        UnpackPrefab = 0xAC,
        GetPhysicsBackends = 0xAD,
        SetPhysicsBackend = 0xAE,
        GetNetworkSettings = 0x51,
        SetNetworkSettings = 0x52,
        GetNetworkValidation = 0x53,
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
        PlaySnapshot = 0xB0,
        PlayState = 0xB1,
        ErrorCoded = 0x1C,
        EditorExtensions = 0x1D,
        EditorUi = 0x1E,
        ExtensionResult = 0x1F,
        LuaSyntax = 0x20,
        AssetPreview = 0xA4,
        SerializedEntities = 0x21,
        EntitiesCreated = 0x22,
        PhysicsBackends = 0x23,
        NetworkSettings = 0x24,
        NetworkValidation = 0x25,
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
        [System.Text.Json.Serialization.JsonPropertyName("state")]
        public string? State { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("frame")]
        public double? Frame { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uiRevision")]
        public double? UiRevision { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("extension")]
        public string? Extension { get; set; }
    }

    public class InputEvent
    {
        [System.Text.Json.Serialization.JsonPropertyName("type")]
        public string Type { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("key")]
        public string? Key { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("button")]
        public string? Button { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("down")]
        public bool? Down { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("x")]
        public double? X { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("y")]
        public double? Y { get; set; }
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
        [System.Text.Json.Serialization.JsonPropertyName("prefab")]
        public string? Prefab { get; set; }
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
        [System.Text.Json.Serialization.JsonPropertyName("prefab")]
        public string? Prefab { get; set; }
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

    public class ExtensionInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("name")]
        public string Name { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("sha256")]
        public string Sha256 { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("apiVersion")]
        public double ApiVersion { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("state")]
        public string State { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("error")]
        public string? Error { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("capabilities")]
        public string[] Capabilities { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("itemCount")]
        public double ItemCount { get; set; }
    }

    public class EditorExtensionsInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("enabled")]
        public bool Enabled { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uiRevision")]
        public double UiRevision { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("extensions")]
        public ExtensionInfo[] Extensions { get; set; }
    }

    public class MenuForm
    {
        [System.Text.Json.Serialization.JsonPropertyName("fields")]
        public FieldSchema[] Fields { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("defaults")]
        public System.Text.Json.JsonElement Defaults { get; set; }
    }

    public class MenuItemInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("extension")]
        public string Extension { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("title")]
        public string Title { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("enabledWhen")]
        public System.Text.Json.JsonElement EnabledWhen { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("confirm")]
        public string? Confirm { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("minRole")]
        public string MinRole { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("form")]
        public MenuForm? Form { get; set; }
    }

    public class InspectorButtonInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("minRole")]
        public string MinRole { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("form")]
        public MenuForm? Form { get; set; }
    }

    public class InspectorSectionInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("componentType")]
        public string ComponentType { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("extension")]
        public string Extension { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("fields")]
        public FieldSchema[] Fields { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("buttons")]
        public InspectorButtonInfo[] Buttons { get; set; }
    }

    public class EditorUiInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("uiRevision")]
        public double UiRevision { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("menuItems")]
        public MenuItemInfo[] MenuItems { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("inspectorSections")]
        public InspectorSectionInfo[] InspectorSections { get; set; }
    }

    public class ExtensionResultInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("message")]
        public string? Message { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("select")]
        public string[]? Select { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("warnings")]
        public string[]? Warnings { get; set; }
    }

    public class PhysicsBackendInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("displayName")]
        public string DisplayName { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("compiledIn")]
        public bool CompiledIn { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("determinism")]
        public string Determinism { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("determinismDefault")]
        public string DeterminismDefault { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("features")]
        public string[] Features { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("missingFeatures")]
        public string[] MissingFeatures { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("supportsSnapshot")]
        public bool SupportsSnapshot { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("supportsSetState")]
        public bool SupportsSetState { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("deterministicSelectable")]
        public bool DeterministicSelectable { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("note")]
        public string? Note { get; set; }
    }

    public class PhysicsBackendsInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("current")]
        public string Current { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("configured")]
        public bool Configured { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("active")]
        public string Active { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("restartRequired")]
        public bool RestartRequired { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("networkPreset")]
        public string NetworkPreset { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("backends")]
        public PhysicsBackendInfo[] Backends { get; set; }
    }

    public class NetworkSettingRow
    {
        [System.Text.Json.Serialization.JsonPropertyName("key")]
        public string Key { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("governed")]
        public bool Governed { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("locked")]
        public bool Locked { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("tunable")]
        public bool Tunable { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("startupChecked")]
        public bool StartupChecked { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("settingKey")]
        public string? SettingKey { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("value")]
        public System.Text.Json.JsonElement Value { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("description")]
        public string Description { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("lostGuarantee")]
        public string? LostGuarantee { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("current")]
        public System.Text.Json.JsonElement? Current { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("atPresetValue")]
        public bool? AtPresetValue { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("source")]
        public string Source { get; set; }
    }

    public class NetworkPresetInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("summary")]
        public string Summary { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("selected")]
        public bool Selected { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("selectable")]
        public bool Selectable { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("unavailableReason")]
        public string? UnavailableReason { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("rows")]
        public NetworkSettingRow[] Rows { get; set; }
    }

    public class NetworkNudge
    {
        [System.Text.Json.Serialization.JsonPropertyName("show")]
        public bool Show { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("reasons")]
        public string[] Reasons { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("networkObjects")]
        public double NetworkObjects { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("hint")]
        public string Hint { get; set; }
    }

    public class NetworkSettingRange
    {
        [System.Text.Json.Serialization.JsonPropertyName("key")]
        public string Key { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("min")]
        public double Min { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("max")]
        public double Max { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("default")]
        public double Default { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("integer")]
        public bool Integer { get; set; }
    }

    public class NetworkLostGuarantee
    {
        [System.Text.Json.Serialization.JsonPropertyName("settingKey")]
        public string SettingKey { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("rowKey")]
        public string RowKey { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("text")]
        public string Text { get; set; }
    }

    public class NetworkBackendInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("selected")]
        public string Selected { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("deterministicSelectable")]
        public string[] DeterministicSelectable { get; set; }
    }

    public class NetworkSettingsInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("preset")]
        public string Preset { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("customised")]
        public bool Customised { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("storedCustomised")]
        public bool StoredCustomised { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("tickRate")]
        public double TickRate { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("values")]
        public System.Text.Json.JsonElement Values { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("overrides")]
        public System.Text.Json.JsonElement Overrides { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("ranges")]
        public NetworkSettingRange[] Ranges { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("lostGuarantees")]
        public NetworkLostGuarantee[] LostGuarantees { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("presets")]
        public NetworkPresetInfo[] Presets { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("backend")]
        public NetworkBackendInfo Backend { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("nudge")]
        public NetworkNudge Nudge { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("problems")]
        public string[] Problems { get; set; }
    }

    public class NetworkRequirementFix
    {
        [System.Text.Json.Serialization.JsonPropertyName("label")]
        public string Label { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("settings")]
        public System.Text.Json.JsonElement Settings { get; set; }
    }

    public class NetworkRequirementInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("id")]
        public string Id { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("text")]
        public string Text { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("met")]
        public bool Met { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("detail")]
        public string Detail { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("fix")]
        public NetworkRequirementFix? Fix { get; set; }
    }

    public class NetworkValidationReport
    {
        [System.Text.Json.Serialization.JsonPropertyName("preset")]
        public string Preset { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("projectPreset")]
        public string ProjectPreset { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("refused")]
        public bool Refused { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("unmet")]
        public double Unmet { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("summary")]
        public string Summary { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("requirements")]
        public NetworkRequirementInfo[] Requirements { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("warnings")]
        public string[] Warnings { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("environment")]
        public System.Text.Json.JsonElement Environment { get; set; }
    }

    public class ClipboardPayload
    {
        [System.Text.Json.Serialization.JsonPropertyName("format")]
        public string Format { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("version")]
        public double Version { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("objectCount")]
        public double ObjectCount { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("entities")]
        public System.Text.Json.JsonElement[] Entities { get; set; }
    }

    public class LuaSyntaxResult
    {
        [System.Text.Json.Serialization.JsonPropertyName("ok")]
        public bool Ok { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("line")]
        public double Line { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("column")]
        public double? Column { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("message")]
        public string Message { get; set; }
    }

    public class AudioPeak
    {
        [System.Text.Json.Serialization.JsonPropertyName("min")]
        public double Min { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("max")]
        public double Max { get; set; }
    }

    public class AssetPreviewInfo
    {
        [System.Text.Json.Serialization.JsonPropertyName("kind")]
        public string Kind { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("uuid")]
        public string Uuid { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("path")]
        public string Path { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("modified")]
        public double Modified { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("width")]
        public double? Width { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("height")]
        public double? Height { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("sourceWidth")]
        public double? SourceWidth { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("sourceHeight")]
        public double? SourceHeight { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("durationMs")]
        public double? DurationMs { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("channels")]
        public double? Channels { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("sampleRate")]
        public double? SampleRate { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("peaks")]
        public AudioPeak[]? Peaks { get; set; }
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
        /// <summary>Optional trailing: the default (false, 0 or empty) when the payload ends before it</summary>
        public bool ErrorCodes;
        /// <summary>Optional trailing: the default (false, 0 or empty) when the payload ends before it</summary>
        public string User;
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

    public struct SerializeEntitiesRequest
    {
        /// <summary>JSON text: any JSON value</summary>
        public string Ids;
    }

    public struct InstantiateEntitiesRequest
    {
        public string Clipboard;
        public string ParentId;
        public int SiblingIndex;
        public bool KeepExternalRefs;
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

    public struct InvokeMenuItemRequest
    {
        public string ItemId;
        /// <summary>JSON text: any JSON value</summary>
        public string Selection;
        /// <summary>JSON text: any JSON value</summary>
        public string Params;
        public uint UiRevision;
    }

    public struct InvokeInspectorActionRequest
    {
        public string ComponentId;
        public string ActionId;
        /// <summary>JSON text: any JSON value</summary>
        public string Params;
        public uint UiRevision;
    }

    public struct BeginEditGroupRequest
    {
        public string Label;
    }

    public struct WritePlaySnapshotRequest
    {
        public string ScenePath;
    }

    public struct SetPausedRequest
    {
        public bool Paused;
    }

    public struct StepRequest
    {
        public uint Frames;
    }

    public struct SendInputRequest
    {
        /// <summary>JSON text: InputEvent[]</summary>
        public string Events;
    }

    public struct SetEditorViewOptionsRequest
    {
        public bool Wireframe;
    }

    public struct CheckLuaSyntaxRequest
    {
        public string Text;
        public string ChunkName;
    }

    public struct GetAssetPreviewRequest
    {
        public string UuidOrPath;
        public uint MaxSize;
    }

    public struct CreatePrefabRequest
    {
        public string EntityId;
        public string Path;
        public bool Overwrite;
    }

    public struct InstantiatePrefabRequest
    {
        public string UuidOrPath;
        public string ParentId;
        public int SiblingIndex;
        public bool HasPosition;
        public Vec3 Position;
    }

    public struct ApplyPrefabRequest
    {
        public string EntityId;
    }

    public struct UnpackPrefabRequest
    {
        public string EntityId;
    }

    public struct SetPhysicsBackendRequest
    {
        public string Backend;
    }

    public struct SetNetworkSettingsRequest
    {
        public string Preset;
        /// <summary>JSON text: any JSON value</summary>
        public string Settings;
    }

    public struct GetNetworkValidationRequest
    {
        public string Preset;
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
        /// <summary>Optional trailing: the default (false, 0 or empty) when the payload ends before it</summary>
        public bool ReadOnly;
        /// <summary>Optional trailing: the default (false, 0 or empty) when the payload ends before it</summary>
        public string User;
        /// <summary>Optional trailing: the default (false, 0 or empty) when the payload ends before it</summary>
        public string Role;
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

    public struct SerializedEntitiesResponse
    {
        /// <summary>JSON text: ClipboardPayload</summary>
        public string Clipboard;
    }

    public struct EntitiesCreatedResponse
    {
        /// <summary>JSON text: string[]</summary>
        public string Ids;
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

    public struct EditorExtensionsResponse
    {
        /// <summary>JSON text: EditorExtensionsInfo</summary>
        public string Info;
    }

    public struct EditorUiResponse
    {
        /// <summary>JSON text: EditorUiInfo</summary>
        public string Ui;
    }

    public struct ExtensionResultResponse
    {
        public string Label;
        public uint Revision;
        public bool CanUndo;
        public bool CanRedo;
        public uint SavedRevision;
        /// <summary>JSON text: ExtensionResultInfo</summary>
        public string Result;
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

    public struct PlaySnapshotResponse
    {
        public string File;
    }

    public struct PlayStateResponse
    {
        public string State;
        public uint Frame;
        public float Time;
    }

    public struct LuaSyntaxResponse
    {
        /// <summary>JSON text: LuaSyntaxResult</summary>
        public string Result;
    }

    public struct AssetPreviewResponse
    {
        /// <summary>JSON text: AssetPreviewInfo</summary>
        public string Info;
        public byte[] Pixels;
    }

    public struct PhysicsBackendsResponse
    {
        /// <summary>JSON text: PhysicsBackendsInfo</summary>
        public string Info;
    }

    public struct NetworkSettingsResponse
    {
        /// <summary>JSON text: NetworkSettingsInfo</summary>
        public string Info;
    }

    public struct NetworkValidationResponse
    {
        /// <summary>JSON text: NetworkValidationReport</summary>
        public string Report;
    }

    public struct ErrorCodedResponse
    {
        public string Message;
        public string Code;
    }

}
