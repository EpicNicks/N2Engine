#pragma once

#include <expected>
#include <filesystem>
#include <functional>
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

    /// The extension of a game's pack (a zip or 7z file; docs/packs.html): n2build writes game.n2pack beside the player
    inline constexpr std::string_view PackExtension = ".n2pack";

    /// Where the game is: its folder, and the pack to mount there (empty for a folder on disk)
    struct GameLocation
    {
        /// The folder the engine opens as the project; with a pack, a path inside no real folder, where the pack is mounted
        std::filesystem::path projectDir;
        std::filesystem::path pack;
    };

    /**
     * The game a player runs, from its folder argument (or the default "game" folder beside it): a folder on disk; a pack
     * named directly (game.n2pack, mounted at "game" beside it); or, when the folder is not there, the pack beside it with
     * the same name and .n2pack. A folder that exists wins over a pack (a developer's unpacked game).
     */
    GameLocation ResolveGame(const std::filesystem::path &folderOrPack,
                             const std::function<bool(const std::filesystem::path &)> &isDirectory,
                             const std::function<bool(const std::filesystem::path &)> &isFile);

    /// "OpenGL", "Vulkan" or "software", for the window title and messages
    std::string_view RenderBackendTitle(RenderBackend backend);
}
