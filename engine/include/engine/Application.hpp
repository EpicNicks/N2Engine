#pragma once

#include <memory>
#include <optional>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/Health.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/input/PointerDispatcher.hpp"

namespace N2Engine
{
    /// What one Application::Tick does besides simulating
    struct TickOptions
    {
        /// Poll the window's events and update the input system (Window::PollEvents) at the start of the frame
        bool pollEvents = true;
        /// Draw the frame (and present it) at the end. A host that renders on request (the editor's play child)
        /// turns it off and calls RenderGameFrame when a client asks for a picture.
        bool render = true;
        /// Advance time by exactly this many seconds (a step) instead of reading the clock. nullopt: the clock.
        std::optional<double> deltaSeconds;
    };

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
        // The fixed-step loop's catch-up time, carried from one Tick to the next
        double _fixedTimestepAccumulator = 0.0;

    private:
        /// Installs the UI hit provider (UI::UISystem) on the pointer dispatcher
        Application();
        void Render();
        /// Draws the current scene as seen by `camera`, and, when `screenSpaceUI` is set, the screen-space UI pass over it
        /// (what Render does with the main camera). World-space canvases are part of the scene and always draw.
        void Render(const Camera &camera, bool screenSpaceUI = true);
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

        /**
         * One frame of the game, exactly the body of Run()'s loop (which calls it until the window closes or Quit): window
         * events, Time::Update, the fixed steps (physics, FixedUpdate), the pointer dispatch, Update, coroutines,
         * LateUpdate, audio, the frame, then destroyed objects and a pending scene change. A host with its own loop (the
         * editor's play child, which has to answer its client between frames) calls this instead of Run().
         * Without a current scene it still advances time and draws.
         */
        void Tick(const TickOptions &options = TickOptions{});
        /// Makes the next Tick measure its frame from now rather than from the last Tick: call it when ticking resumes
        /// after a pause, so the pause isn't one long frame
        void ResetFrameClock();
        /// Draws one frame of the current scene from the main camera (with the screen-space UI), without simulating,
        /// polling events or advancing time: what a Tick with render off leaves undone. Does nothing without a window
        /// and a renderer.
        void RenderGameFrame();
        /// Shuts subsystems down in reverse order of Init. Run() and Quit() call it; hosts that don't use
        /// Run() (e.g. the editor) should call it before returning from main, so teardown doesn't depend
        /// on static destruction order. Safe to call more than once, or without Init.
        void Shutdown();
        /// Draws one frame of the current scene from the main camera, without simulating (the editor's RenderFrame)
        void RenderEditorFrame();
        /**
         * Draws one frame of the current scene as seen by `camera`, without simulating: the editor's viewport, which
         * has a camera of its own (the editor camera) and so needs neither a Camera component in the scene nor the
         * game's main camera. It differs from RenderEditorFrame() in the camera (the view and projection, and the
         * position the lit shaders see) and in two ways that suit a scene view: it draws no screen-space UI (the game's
         * HUD overlay; world-space canvases are scene objects and still draw), and it doesn't poll window events or
         * read input. Does nothing without a window and a renderer.
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
