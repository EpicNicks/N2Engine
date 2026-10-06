// ResourceUUID.hpp
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <math/UUID.hpp>
#include "ResourcePath.hpp"

namespace N2Engine::IO
{
    /**
     * Generates deterministic UUIDs for resources
     * Uses project-specific namespace to prevent collisions
     */
    class ResourceUUID
    {
    public:
        /**
         * Initialize with project namespace UUID
         * Should be called once during ResourceLoader initialization
         */
        static void Initialize(const Math::UUID& projectNamespace);

        /**
         * The project namespace for a project folder, shared by every tool that opens one (lua_project, the
         * editor host), so a folder's assets get the same UUIDs whichever opened it:
         * GenerateNameBased(UUID::ZERO, NormalizeProjectDir(projectDir)). Until projects carry their own id.
         */
        static Math::UUID NamespaceForProjectDir(const std::filesystem::path& projectDir);

        /**
         * The spelling of a project folder that NamespaceForProjectDir hashes: weakly_canonical (absolute, "."
         * and ".." resolved, symlinks followed where the path exists), with forward slashes (generic_string), no
         * trailing slash and, on Windows, an upper-case drive letter. That is how CMake writes a source path
         * (C:/...), so lua_project's default N2_LUA_PROJECT_DIR keeps the namespace it always had.
         */
        static std::string NormalizeProjectDir(const std::filesystem::path& projectDir);

        /**
         * Generate deterministic UUID from resource path
         */
        static Math::UUID FromPath(const ResourcePath& path);

        /**
         * The deterministic UUID of the sub-asset `key` ("mesh/Body") inside the file `parentPath` (a model's mesh,
         * material or texture). Name-based in a namespace of its own (derived from the project's), so it never
         * equals a file's FromPath UUID, whatever the file is called. The same parent and key always give the same
         * UUID, so scene files save it like any other asset UUID.
         */
        static Math::UUID FromSubAsset(const ResourcePath& parentPath, std::string_view key);

        /**
         * Get the current project namespace
         */
        static const Math::UUID& GetProjectNamespace();

    private:
        static Math::UUID s_projectNamespace;
        static bool s_initialized;
    };
}