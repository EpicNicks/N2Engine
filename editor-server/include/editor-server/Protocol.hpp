#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace N2Engine::Editor
{
    /// The version of protocol.json this server implements (ProtocolSpecTest pins the two equal). Hello compares the
    /// client's with it: the same major version is compatible, whatever the minor and patch versions.
    inline constexpr std::string_view ProtocolVersion = "1.3.0";

    struct ProtocolVersionNumber
    {
        uint32_t majorVersion = 0;
        uint32_t minorVersion = 0;
        uint32_t patchVersion = 0;
    };

    /// The most digits a part of a protocol version may have (a uint32 has at most 10)
    inline constexpr std::size_t MaxProtocolVersionPartDigits = 10;

    /// "major.minor.patch", each part 1 to 10 decimal digits that fit a uint32; nullopt for anything else (signs,
    /// spaces, missing or extra parts, or a part padded with zeros past 10 digits, which from_chars would accept)
    [[nodiscard]] inline std::optional<ProtocolVersionNumber> ParseProtocolVersion(std::string_view text)
    {
        ProtocolVersionNumber version;
        uint32_t *parts[] = {&version.majorVersion, &version.minorVersion, &version.patchVersion};
        const char *position = text.data();
        const char *const end = text.data() + text.size();
        for (uint32_t *part : parts)
        {
            if (part != parts[0])
            {
                if (position == end || *position != '.')
                    return std::nullopt;
                ++position;
            }
            // from_chars would accept no sign anyway, but this also rules out an empty part
            if (position == end || *position < '0' || *position > '9')
                return std::nullopt;
            const auto [next, error] = std::from_chars(position, end, *part);
            if (error != std::errc{} || static_cast<std::size_t>(next - position) > MaxProtocolVersionPartDigits)
                return std::nullopt;
            position = next;
        }
        if (position != end)
            return std::nullopt;
        return version;
    }

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
        // 0x28 is kept for GetHierarchy (#6, E4)
        GetOpenScene = 0x29,
        CreateEntity = 0x30,
        DestroyEntity = 0x31,
        SetEntityTransform = 0x32,
        GetEntityTransform = 0x33,
        GetAllEntities = 0x34,
        CreateScript = 0x40,
        RescanAssets = 0x41,
        GetEngineHealth = 0x50,
        GetProjectInfo = 0x70,
        SetProjectSettings = 0x71,
        SetStartupScene = 0x72,
        Shutdown = 0xff
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
        ProjectInfo = 0x0E
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

    struct FrameDataHeader
    {
        uint32_t width;
        uint32_t height;
        // followed by width * height * 4 bytes of RGBA8 pixel data, top row first, alpha 255, on every backend
        // (EditorServer::ReadFrame); width x height is the viewport size SetViewportSize set (default 1280x720)
    };

#pragma pack(pop)
}
