#pragma once
#include <string>

namespace N2Engine::Config
{
    struct ApplicationOptions
    {
        enum class PhysicsBackend
        {
            PHYSX
        };

        enum class RenderBackend
        {
            OPENGL,
            VULKAN
        };

        std::string projectPath;
        PhysicsBackend physicsBackend;
        RenderBackend renderBackend;
        bool isHeadless = false;
    };
}
