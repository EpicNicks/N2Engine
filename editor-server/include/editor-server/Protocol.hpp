#pragma once

#include <cstdint>

namespace N2Engine::Editor
{
    enum class CommandType : uint8_t
    {
        RenderFrame = 0x01,
        SetViewportSize = 0x02,

        SetCameraPosition = 0x10,
        SetCameraRotation = 0x11,
        GetCameraPosition = 0x12,

        // Scene management
        CreateScene = 0x20,
        LoadScene = 0x21,
        SaveScene = 0x22,
        DeleteScene = 0x23,
        GetCurrentScene = 0x24,

        // Entity management
        CreateEntity = 0x30,
        DestroyEntity = 0x31,
        SetEntityTransform = 0x32,
        GetEntityTransform = 0x33,
        GetAllEntities = 0x34,

        StepSimulation = 0x41,

        Shutdown = 0xFF
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
        SceneInfo = 0x07
    };

#pragma pack(push, 1)

    struct Vec3
    {
        float x, y, z;
    };

    struct SetViewportSizeRequest
    {
        int32_t width;
        int32_t height;
    };

    struct SetCameraPositionRequest
    {
        float x, y, z;
    };

    struct EntityTransformRequest
    {
        int32_t entityId;
        Vec3 position;
        Vec3 rotation;
        Vec3 scale;
    };

    struct FrameDataHeader
    {
        uint32_t width;
        uint32_t height;
        // followed by width * height * 4 bytes of BGRA pixel data
    };

#pragma pack(pop)
}
