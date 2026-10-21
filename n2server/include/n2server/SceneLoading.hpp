#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace N2Engine::Server
{
    /// A scene loaded by LoadSceneForRun
    struct LoadedScene
    {
        /// The scene's resource path ("res://scenes/main.scene")
        std::string resourcePath;
        /// Network::HashSceneJson of the scene file's parsed JSON: the same for the same content whatever the file's
        /// line endings or key order, so a server and a client that load the same file agree (the fingerprint's scene hash)
        std::uint64_t contentHash = 0;
    };

    /**
     * Loads the scene `scenePath` (a res:// path inside the project) as a running game: built and made current, its
     * components attach at the first tick. The path is resolved like the editor host resolves one (inside the project's
     * assets folder, links resolved). The engine must be initialised and the project's resources set up (as RunServer
     * does before it calls this). Returns what was loaded, or what went wrong (the file is missing, too deeply nested,
     * not JSON, or not a scene). N2Server and the test client of the loopback tests both use it, so a server and a
     * client built over it load a scene the same way.
     */
    [[nodiscard]] std::expected<LoadedScene, std::string> LoadSceneForRun(const std::filesystem::path &projectDir,
                                                                          const std::string &scenePath);
}
