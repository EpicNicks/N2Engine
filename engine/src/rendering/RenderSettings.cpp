#include "engine/rendering/RenderSettings.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>

namespace N2Engine::Rendering
{
    namespace
    {
        std::atomic<ColorSpace> &Current()
        {
            static std::atomic<ColorSpace> colorSpace{ColorSpace::Gamma};
            return colorSpace;
        }
    }

    namespace
    {
        std::atomic<bool> &Force()
        {
            static std::atomic<bool> force{false};
            return force;
        }
    }

    namespace
    {
        std::atomic<UserShaderErrorStyle> &ErrorStyle()
        {
            static std::atomic<UserShaderErrorStyle> style{UserShaderErrorStyle::Unlit};
            return style;
        }
    }

    namespace
    {
        std::atomic<bool> &RuntimeCompile()
        {
            static std::atomic<bool> enabled{false};
            return enabled;
        }

        std::mutex &CacheMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::filesystem::path &CacheDirectory()
        {
            static std::filesystem::path directory;
            return directory;
        }
    }

    bool RenderSettings::GetRuntimeShaderCompile()
    {
        return RuntimeCompile().load();
    }

    void RenderSettings::SetRuntimeShaderCompile(const bool enabled)
    {
        RuntimeCompile().store(enabled);
    }

    std::filesystem::path RenderSettings::GetShaderCacheDirectory()
    {
        const std::lock_guard lock(CacheMutex());
        return CacheDirectory();
    }

    void RenderSettings::SetShaderCacheDirectory(std::filesystem::path directory)
    {
        const std::lock_guard lock(CacheMutex());
        CacheDirectory() = std::move(directory);
    }

    UserShaderErrorStyle RenderSettings::GetUserShaderErrorStyle()
    {
        return ErrorStyle().load();
    }

    void RenderSettings::SetUserShaderErrorStyle(const UserShaderErrorStyle style)
    {
        ErrorStyle().store(style);
    }

    bool RenderSettings::GetForceShaderEncode()
    {
        return Force().load();
    }

    void RenderSettings::SetForceShaderEncode(const bool force)
    {
        Force().store(force);
    }

    ColorSpace RenderSettings::GetColorSpace()
    {
        return Current().load();
    }

    void RenderSettings::SetColorSpace(const ColorSpace colorSpace)
    {
        Current().store(colorSpace);
    }

    std::optional<ColorSpace> RenderSettings::ParseColorSpace(const std::string_view name)
    {
        std::string lowered(name);
        std::ranges::transform(lowered, lowered.begin(),
                               [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "gamma")
        {
            return ColorSpace::Gamma;
        }
        if (lowered == "linear")
        {
            return ColorSpace::Linear;
        }
        return std::nullopt;
    }

    std::string_view RenderSettings::ColorSpaceName(const ColorSpace colorSpace)
    {
        return colorSpace == ColorSpace::Linear ? "linear" : "gamma";
    }
}
