#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/config/ApplicationOptions.hpp"

namespace N2Engine::Player
{
    using RenderBackend = Config::ApplicationOptions::RenderBackend;

    /// The desktop player's command line (players/desktop; docs/build-targets.html#player). Parsed without touching the
    /// engine, so the rules are unit tested (tests/players).
    struct PlayerOptions
    {
        /// The game's folder (project.n2proj and assets/). Empty: the caller's default (the "game" folder beside the
        /// executable in a package).
        std::filesystem::path projectDir;
        RenderBackend renderBackend = RenderBackend::SOFTWARE;
        /// Log lines on stdout in a Release build too (Application::Init installs the console only under N2ENGINE_DEBUG)
        bool consoleLog = false;
        /// Run this many frames, then quit as if the window closed (smoke runs). Absent: until the window closes.
        std::optional<int> frames;
        bool help = false;
    };

    /// The usage text, for --help and after a bad command line
    std::string PlayerUsage(std::string_view programName);

    /// Parses the arguments after the program name. An error names the bad argument; the caller prints it with the usage.
    std::expected<PlayerOptions, std::string> ParsePlayerArguments(std::span<const std::string_view> args);

    /// "OpenGL", "Vulkan" or "software", for the window title and messages
    std::string_view RenderBackendTitle(RenderBackend backend);
}
