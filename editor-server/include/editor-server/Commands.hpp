#pragma once

#include "Protocol.hpp"
#include "Serialization.hpp"
#include <math/Vector3.hpp>

namespace N2Engine::Editor::Protocol
{
    struct SetViewportSizeCmd
    {
        int32_t width;
        int32_t height;

        static SetViewportSizeCmd Deserialize(BufferReader &r)
        {
            return {r.ReadI32(), r.ReadI32()};
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

    struct CreateEntityCmd
    {
        std::string name;

        static CreateEntityCmd Deserialize(BufferReader &r)
        {
            return {r.ReadString()};
        }
    };

    struct EntityTransformCmd
    {
        std::string entityId;
        Math::Vector3 position;
        Math::Vector3 rotation;
        Math::Vector3 scale;

        static EntityTransformCmd Deserialize(BufferReader &r)
        {
            std::string id = r.ReadString();
            Math::Vector3 pos{r.ReadF32(), r.ReadF32(), r.ReadF32()};
            Math::Vector3 rot{r.ReadF32(), r.ReadF32(), r.ReadF32()};
            Math::Vector3 scl{r.ReadF32(), r.ReadF32(), r.ReadF32()};
            return {id, pos, rot, scl};
        }
    };

    // Response builders
    inline void WriteOk(BufferWriter &w)
    {
        w.WriteU8(static_cast<uint8_t>(ResponseType::Ok));
        w.WriteU32(0);
    }

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
}
