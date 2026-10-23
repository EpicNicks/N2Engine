#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "engine/config/ApplicationOptions.hpp"

// The web player's --renderer argument (stage G4 of #155). No Emscripten and no DOM in here, so the web tests run it under
// node (tests/web).
namespace N2Engine::Web
{
    /// The render backend for the value of --renderer: "software" (the default when the argument is absent) or "webgpu",
    /// exactly those lowercase names. A native backend name ("opengl", "vulkan") is an error saying it does not exist in the
    /// browser; anything else (an empty value included) is "Invalid renderer: <value> (expected software or webgpu)".
    /// WebGPU requires the device acquired by the page before callMain. The shell
    /// passes its selection explicitly; direct callers without an argument use software.
    [[nodiscard]] std::expected<Config::ApplicationOptions::RenderBackend, std::string> ParseWebRenderer(std::string_view value);
}
