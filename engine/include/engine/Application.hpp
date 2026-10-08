#pragma once

#include <memory>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/Health.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/input/PointerDispatcher.hpp"

namespace N2Engine
{
    class Application
    {
        friend class SceneManager;

    private:
        Window _window;
        std::unique_ptr<Camera> _mainCamera;
        std::unique_ptr<Physics::IPhysicsBackend> _3DphysicsBackend = nullptr;
        Input::PointerDispatcher _pointerDispatcher;
        EngineHealth _health;
        bool _quitRequested = false;
        bool _initialized = false; // from Init until Shutdown

    private:
        /// Installs the UI hit provider (UI::UISystem) on the pointer dispatcher
        Application();
        void Render();
        /// Draws the current scene (and the UI over it) as seen by `camera`: what Render does with the main camera
        void Render(const Camera &camera);
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
        /// Draws one frame of the current scene from the main camera, without simulating (the editor's RenderFrame)
        void RenderEditorFrame();
        /**
         * Draws one frame of the current scene as seen by `camera`, without simulating: the editor's viewport, which
         * has a camera of its own (the editor camera) and so needs neither a Camera component in the scene nor the
         * game's main camera. Only the camera differs from RenderEditorFrame(): the view and projection, the position
         * the lit shaders see, and nothing else. Does nothing without a window and a renderer.
         */
        void RenderEditorFrame(const Camera &camera);

        /// Requests a quit at the end of the current frame (Run() then calls OnApplicationQuit and
        /// shuts down). Safe to call from components and Lua.
        static void Quit();
        [[nodiscard]] bool IsQuitRequested() const { return _quitRequested; }

        [[nodiscard]] Camera* GetMainCamera() const;
        Window& GetWindow();
        /// Sends OnMouse* to the object under the pointer; Run updates it each frame before Scene::Update. Its UI
        /// hit provider is UI::UISystem's, so UI elements under the pointer get the callbacks before the world.
        Input::PointerDispatcher& GetPointerDispatcher() { return _pointerDispatcher; }

        void OnWindowResize(int width, int height) const;

        [[nodiscard]] Physics::IPhysicsBackend* Get3DPhysicsBackend() const;
        /// Installs a physics backend without Init, for headless tools and tests (Init needs a window).
        /// Replaces and destroys any existing backend; pass nullptr to remove it.
        void Set3DPhysicsBackend(std::unique_ptr<Physics::IPhysicsBackend> backend);

        /// Subsystem states as of the end of Init
        [[nodiscard]] const EngineHealth& GetHealth() const { return _health; }
    };
}
