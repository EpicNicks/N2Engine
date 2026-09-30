#pragma once

#include <memory>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/Health.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

namespace N2Engine
{
    class Application
    {
        friend class SceneManager;

    private:
        Window _window;
        std::unique_ptr<Camera> _mainCamera;
        std::unique_ptr<Physics::IPhysicsBackend> _3DphysicsBackend = nullptr;
        EngineHealth _health;
        bool _quitRequested = false;

    private:
        Application() = default;
        void Render();
        void PhysicsUpdate(const Scene &scene) const;

    public:
        Application(const Application &) = delete;
        Application& operator=(const Application &) = delete;
        static Application& GetInstance();

        /// @returns the state of each subsystem; the engine keeps running with whatever started
        EngineHealth Init();
        EngineHealth Init(const Config::ApplicationOptions &options);
        void Init(std::unique_ptr<Scene> &&initialScene);
        void Run();
        /// Shuts subsystems down in reverse order of Init. Run() and Quit() call it; hosts that don't use
        /// Run() (e.g. the editor) should call it before returning from main, so teardown doesn't depend
        /// on static destruction order. Safe to call more than once, or without Init.
        void Shutdown();
        void RenderEditorFrame();

        /// Requests a quit at the end of the current frame (Run() then calls OnApplicationQuit and
        /// shuts down). Safe to call from components and Lua.
        static void Quit();
        [[nodiscard]] bool IsQuitRequested() const { return _quitRequested; }

        [[nodiscard]] Camera* GetMainCamera() const;
        Window& GetWindow();

        void OnWindowResize(int width, int height) const;

        [[nodiscard]] Physics::IPhysicsBackend* Get3DPhysicsBackend() const;
        /// Installs a physics backend without Init, for headless tools and tests (Init needs a window).
        /// Replaces and destroys any existing backend; pass nullptr to remove it.
        void Set3DPhysicsBackend(std::unique_ptr<Physics::IPhysicsBackend> backend);

        /// Subsystem states as of the end of Init
        [[nodiscard]] const EngineHealth& GetHealth() const { return _health; }
    };
}
