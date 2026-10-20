#pragma once

#include <memory>
#include <optional>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/Health.hpp"
#include "engine/SimulationClock.hpp"
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
        // mutable: GetHealth adds the live ParticleSystems to the Particles row's detail
        mutable EngineHealth _health;
        std::string _particlesBaseDetail; // the Particles row's detail as Init made it (the GPU probe's)
        std::string _softBodiesBaseDetail; // the same for the SoftBodies row
        bool _quitRequested = false;
        static inline bool _scriptQuitAllowed = true;
        static inline bool _scriptQuitWarned = false;
        bool _initialized = false; // from Init until Shutdown
        // Decides how many fixed ticks each Tick runs (its accumulator is the catch-up time carried from one Tick to the
        // next) and owns the tick counter
        SimulationClock _simulationClock;

    private:
        /// Installs the UI hit provider (UI::UISystem) on the pointer dispatcher
        Application();
        void Render();
        /// Draws the current scene as seen by `camera`, and, when `screenSpaceUI` is set, the screen-space UI pass over it
        /// (what Render does with the main camera). World-space canvases are part of the scene and always draw.
        void Render(const Camera &camera, bool screenSpaceUI = true);
        void PhysicsUpdate(const Scene &scene) const;
        /// One fixed tick of `scene`: in None mode the physics step (FixedUpdate, the backend, the transform sync),
        /// unless the time scale is 0; in a network mode the same after the tick counter moves on and the attach queue is
        /// processed. True if a step ran.
        bool RunFixedTick(Scene &scene);

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
         * Run() in three parts, for a host whose loop is not its own to block: the web player, where the browser calls a
         * frame function and a blocking while loop would freeze the tab (emscripten_set_main_loop). Run() is exactly
         * BeginRun, then RunFrame until it returns false, then EndRun, so the frame, the fixed-step accumulator and the
         * ending are the same on every platform.
         * BeginRun: false (after shutting down, with the error logged) when there is no valid window and renderer;
         * otherwise it clears the fixed-step accumulator and returns true.
         * RunFrame: false, without ticking, once the window should close or Quit() was called; otherwise one Tick.
         * EndRun: OnApplicationQuit on the current scene, then Shutdown. Call it once, after RunFrame returned false.
         */
        [[nodiscard]] bool BeginRun();
        [[nodiscard]] bool RunFrame();
        void EndRun();

        /**
         * One frame of the game, exactly the body of Run()'s loop (which calls it until the window closes or Quit): window
         * events, Time::Update, the fixed steps (physics, FixedUpdate), the pointer dispatch, Update, coroutines,
         * LateUpdate, audio, the frame, then destroyed objects and a pending scene change. A host with its own loop (the
         * editor's play child, which has to answer its client between frames) calls this instead of Run().
         * Without a current scene it still advances time and draws.
         */
        void Tick(const TickOptions &options = TickOptions{});
        /**
         * How the fixed-step loop runs (see SimulationClock): None, the default, is the offline loop exactly as it has
         * always been. Authoritative and Deterministic are the tick-owned loop of the network presets: the fixed delta
         * ignores the time scale (which then only scales what Update sees), the attach queue is processed at each tick
         * boundary instead of once per frame, and no frame time is dropped (a clock too far behind reports it through
         * the SimulationClock's behind handler). The network presets will set this when the project's settings do;
         * until then a host or a test does. Does not touch the tick counter; going to None keeps at most 0.25 s of carried time.
         */
        void SetSimulationMode(SimulationMode mode);
        [[nodiscard]] SimulationMode GetSimulationMode() const { return _simulationClock.GetMode(); }
        /// The clock that decides the ticks: its catch-up limit, behind handler and accumulator
        [[nodiscard]] SimulationClock& GetSimulationClock() { return _simulationClock; }
        [[nodiscard]] const SimulationClock& GetSimulationClock() const { return _simulationClock; }
        /**
         * Runs exactly `count` fixed ticks of the current scene, headless: no window events, no clock, no Update,
         * coroutines, LateUpdate, drawing or destroyed-object cleanup, and the accumulator is not touched. Each tick is
         * the one a Tick runs. In None mode the attach queue is processed once first (as a frame would) and no tick runs
         * while the time scale is 0; a network mode processes it at every tick boundary and has no such pause. Returns the
         * ticks that ran (0 without a current scene).
         */
        std::uint32_t StepTicks(std::uint32_t count);
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
        /// Forgets a quit request (Init does too). For tests, which share one Application for the whole process.
        void ResetQuitRequest() { _quitRequested = false; }

        /// Whether Lua's Application.Quit() does anything. It does in a game (the default) and in the editor's --play
        /// child, which is a game; the editor host that edits (N2EditorHost without --play) clears it, because a script
        /// in a scene a client loaded must not be able to end the host. A refused call logs one warning and does
        /// nothing; C++ callers of Quit() are not affected. Setting it also re-arms that warning.
        static void SetScriptQuitAllowed(bool allowed);
        [[nodiscard]] static bool IsScriptQuitAllowed();
        /// What the Lua binding of Application.Quit calls: Quit() if scripts may quit, else one warning
        static void QuitFromScript();

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

        /// Subsystem states as of the end of Init. The Particles row's state is the GPU probe's with the particle
        /// feature's own failure on top (see Physics::MakeParticlesStatus); once ParticleSystems exist its detail
        /// also says which backends they use ("ParticleSystems: 2 CPU"). With none, the row is exactly what Init
        /// made. The SoftBodies row (Physics::MakeSoftBodiesStatus) is built the same way from its own failure
        /// state: a particle failure never changes it and the reverse.
        [[nodiscard]] const EngineHealth& GetHealth() const;
    };
}
