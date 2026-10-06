// ResourceUUID.hpp
#pragma once

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