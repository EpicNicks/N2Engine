// ResourceUUID.hpp
#pragma once

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
         * Get the current project namespace
         */
        static const Math::UUID& GetProjectNamespace();

    private:
        static Math::UUID s_projectNamespace;
        static bool s_initialized;
    };
}