#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/io/Vfs.hpp"

namespace N2Engine::IO
{
    /**
     * Folders of the game whose files arrive after the start (#425, docs/web-build.html#late-packs): on the web, a project
     * lists folders in build.web.lateAssets, the package leaves them out of its first download and the loader fetches each
     * as a pack once the game runs. Until a folder's pack arrives its files are not there: a load of one fails with a message
     * that names the pack, and a game asks IsAvailable (Lua Assets.IsAvailable) before it loads a scene from such a folder.
     * When the pack arrives it is mounted at the folder (Vfs) and the project's assets are rescanned, so its files load as
     * any other. Main thread only, like the loader. Nothing is registered outside the web player: everything is available.
     */
    namespace LatePacks
    {
        /// A folder whose files come later (absolute; lexically normalised here)
        void Register(const std::filesystem::path &folder);
        /// Whether `path` is inside a registered folder whose pack has not arrived
        [[nodiscard]] bool IsPending(const std::filesystem::path &path);
        /// The registered folders still waiting for their pack
        [[nodiscard]] std::vector<std::filesystem::path> Pending();
        /// The registered folders whose pack has arrived (mounted): the asset scan lists their files with the folder's
        /// parent on disk, which a walk of the disk alone would not see
        [[nodiscard]] std::vector<std::filesystem::path> Arrived();
        /// The pack of a registered folder arrived: it is mounted at the folder and the folder is no longer pending, and
        /// ResourceLoader rescans the project's assets when it is initialised. False (nothing changes) for a folder that was
        /// not registered or has arrived already.
        bool Arrive(const std::filesystem::path &folder, std::unique_ptr<IMount> mount);
        /// Forgets every folder (tests; a player that opens another project)
        void Reset();
    }
}
