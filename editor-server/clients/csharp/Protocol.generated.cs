// Auto-generated from protocol.json - do not edit
using System;

namespace N2Engine.Editor.Protocol
{
    public enum CommandType : byte
    {
        RenderFrame = 0x01,
        SetViewportSize = 0x02,
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
    }

    public struct vec3
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct EntityInfo
    {
        public string Id;
        public string Name;
    }

    public struct SetViewportSizeRequest
    {
        public int Width;
        public int Height;
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
        public string Scenejson;
    }

    public struct DeleteSceneRequest
    {
        public string Scenename;
    }

    public struct CreateEntityRequest
    {
        public string Name;
    }

    public struct DestroyEntityRequest
    {
        public string Entityid;
    }

    public struct SetEntityTransformRequest
    {
        public string Entityid;
        public Vec3 Position;
        public Vec3 Rotation;
        public Vec3 Scale;
    }

    public struct GetEntityTransformRequest
    {
        public string Entityid;
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

    public struct CameraPositionResponse
    {
        public float X;
        public float Y;
        public float Z;
    }

    public struct SceneDataResponse
    {
        public string Scenejson;
    }

    public struct EntityCreatedResponse
    {
        public string Entityid;
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
        public string Scripttemplate;
    }

}