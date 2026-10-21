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
            SOFTWARE,
            /// The WebGPU backend (#155): only in the web build, and only windowless (isHeadless), as the web player's
            /// --renderer webgpu. Natively the backend is a CI test harness and Window refuses it (design decision W8). Until
            /// stage G5 hands the page's device in, the browser build cannot start it either (Window::InitWindow fails)
            WEBGPU,
            /// No renderer and no window at all (a dedicated server, N2Server): Window::InitWindow creates neither, so
            /// Window::IsValid() stays false and nothing is ever drawn. Meant with isHeadless; GLFW is not initialised.
            NONE
        };

        std::string projectPath;
        PhysicsBackend physicsBackend;
        RenderBackend renderBackend;
        bool isHeadless = false;
        /// false: no audio device is opened at all, not even the loopback one a headless host streams to its client
        /// (a dedicated server plays nothing). The Audio row of the health report says Disabled.
        bool audio = true;
        /// false: physics.gpu is ignored and the physics scene is built without the GPU (a dedicated server never needs
        /// CUDA, and a network preset's requirements are then checked against a GPU that is off). The project's own
        /// setting is read otherwise.
        bool physicsGpu = true;
    };
}
