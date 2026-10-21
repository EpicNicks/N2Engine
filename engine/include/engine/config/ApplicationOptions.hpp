#pragma once
#include <string>

namespace N2Engine::Config
{
    struct ApplicationOptions
    {
        /// The host's preference for the 3D physics backend. A project's own physics.backend setting wins whenever it
        /// is present (Application::Init reads it); this is what applies when the project does not say. PHYSX and JOLT
        /// name that default, AUTO means no preference (PhysX).
        enum class PhysicsBackend
        {
            PHYSX,
            JOLT,
            AUTO
        };

        enum class RenderBackend
        {
            OPENGL,
            VULKAN,
            SOFTWARE
        };

        std::string projectPath;
        PhysicsBackend physicsBackend;
        RenderBackend renderBackend;
        bool isHeadless = false;
    };
}
