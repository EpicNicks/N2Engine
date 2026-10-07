// Auto-generated from protocol.json by generate_csharp.py - do not edit
// Declarations only, with no encoding code; nothing uses or tests this client (unsupported).
using System;

namespace N2Engine.Editor.Protocol
{
    public static class ProtocolInfo
    {
        /// <summary>protocol.json's version (major.minor.patch); Hello sends it</summary>
        public const string ProtocolVersion = "1.2.0";
    }

    public enum CommandType : byte
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
        CreateEntity = 0x30,
        DestroyEntity = 0x31,
        SetEntityTransform = 0x32,
        GetEntityTransform = 0x33,
        GetAllEntities = 0x34,
        CreateScript = 0x40,
        RescanAssets = 0x41,
        GetEngineHealth = 0x50,
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

    public struct SetCameraPositionRequest
    {
        public float X;
        public float Y;
        public float Z;
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

    public struct CreateScriptRequest
    {
        public string Name;
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

    public struct CameraPositionResponse
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct SceneDataResponse
    {
        public string SceneJson;
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

    public struct ScriptDataResponse
    {
        public string ScriptTemplate;
    }

    public struct EngineHealthResponse
    {
        public bool Healthy;
        public uint Count;
        public SubsystemStatus[] Subsystems;
    }

}
