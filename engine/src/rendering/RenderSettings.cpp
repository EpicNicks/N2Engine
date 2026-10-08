#include "engine/rendering/RenderSettings.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
