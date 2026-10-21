#pragma once
#include <string>

namespace N2Engine::Config
{
    struct ApplicationOptions
    {
        /// The host's preference for the 3D physics backend. A project's own physics.backend setting wins whenever it
        /// is present (Application::Init reads it); this is what applies when the project does not say. PHYSX and JOLT
        /// name that default (a build without it falls back to the other backend when that one is built, except for a
        /// Deterministic project, which is refused); AUTO means no preference: PhysX when it is in the build, else
        /// Jolt, else none (Physics::ChooseBackend).
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
