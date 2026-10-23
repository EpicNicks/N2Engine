#include "WebRendererOption.hpp"

#include <format>

namespace N2Engine::Web
{
    std::expected<Config::ApplicationOptions::RenderBackend, std::string> ParseWebRenderer(const std::string_view value)
    {
        if (value == "software")
        {
            return Config::ApplicationOptions::RenderBackend::SOFTWARE;
        }
        if (value == "webgpu")
        {
            return Config::ApplicationOptions::RenderBackend::WEBGPU;
        }
        if (value == "opengl" || value == "vulkan")
        {
            return std::unexpected(std::format("Invalid renderer: {} is a native renderer, not in the web player "
                                               "(expected software or webgpu)",
                                               value));
        }
        return std::unexpected(std::format("Invalid renderer: {} (expected software or webgpu)", value));
    }
}
