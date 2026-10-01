#include <string>
#include <memory>
#include <algorithm>

#include <math/MathRegistrar.hpp>

#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/Logger.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;

Application& Application::GetInstance()
{
    static Application instance;
    return instance;
}

Window& Application::GetWindow()
{
    return _window;
}

Camera* Application::GetMainCamera() const
{
    return _mainCamera.get();
}

///
///@details Inits with default data:
/// <b>projectPath</b>: "" (unset)
/// <b>physicsBackend</b>: PhysX
/// <b>renderBackend</b>: OpenGL
namespace
{
    std::string_view RenderBackendName(Config::ApplicationOptions::RenderBackend backend)
    {
        switch (backend)
        {
        case Config::ApplicationOptions::RenderBackend::OPENGL:
            return "OpenGL";
        case Config::ApplicationOptions::RenderBackend::VULKAN:
            return "Vulkan";
        case Config::ApplicationOptions::RenderBackend::SOFTWARE:
            return "Software";
        }
        return "Unknown";
    }
}

EngineHealth Application::Init()
{
    return Init({
        .projectPath = "",
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
        .renderBackend = Config::ApplicationOptions::RenderBackend::OPENGL,
        .isHeadless = false,
    });
}

EngineHealth Application::Init(const Config::ApplicationOptions &options)
{
    // A second Init re-created the camera and physics backend under live scenes and opened another window
    if (_initialized)
    {
        Logger::Warn("Application::Init called again; ignored (call Shutdown first to re-initialize)");
        return _health;
    }
    _initialized = true;

#ifdef N2ENGINE_DEBUG
    Logger::InitializeDebugConsoleHelper();
#endif
    _health = {};
    _quitRequested = false;
    SubsystemStatus windowStatus{.name = "Window"};
    SubsystemStatus rendererStatus{.name = "Renderer"};
    SubsystemStatus physicsStatus{.name = "Physics"};
    SubsystemStatus audioStatus{.name = "Audio"};
    SubsystemStatus scriptingStatus{.name = "Scripting"};

    if (Scripting::LuaRuntime::Instance().Initialize())
    {
        scriptingStatus.state = SubsystemState::Running;
    }
    else
    {
        scriptingStatus.state = SubsystemState::Failed;
        scriptingStatus.detail = "Lua bindings failed to register (see log)";
    }

    Math::InitializeSIMD();
    Time::Init();

    const std::string rendererName{RenderBackendName(options.renderBackend)};
    if (_window.InitWindow(options))
    {
        windowStatus.state = SubsystemState::Running;
        rendererStatus.state = SubsystemState::Running;
        rendererStatus.detail = rendererName;
    }
    else if (_window.RendererFailed())
    {
        windowStatus.state = SubsystemState::Failed;
        windowStatus.detail = "Closed because the renderer failed";
        rendererStatus.state = SubsystemState::Failed;
        rendererStatus.detail = rendererName + ": " + _window.GetInitError();
    }
    else
    {
        windowStatus.state = SubsystemState::Failed;
        windowStatus.detail = _window.GetInitError();
        rendererStatus.detail = "Not started because the window failed";
    }

    // Created even without a window so game code can rely on GetMainCamera()
    _mainCamera = std::make_unique<Camera>();

    const Vector2i windowDimensions = _window.GetWindowDimensions();
    const float aspect = windowDimensions[0] > 0 && windowDimensions[1] > 0
                             ? static_cast<float>(windowDimensions[0]) / static_cast<float>(windowDimensions[1])
                             : 16.0f / 9.0f;

    _mainCamera->SetPerspective(45.0f, aspect, 0.1f, 100.0f);
    // _mainCamera->SetOrthographic(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f);
    _mainCamera->SetPosition(Math::Vector3{0.0f, 0.0f, 10.0f});

    Logger::Info("Camera initialized");

#ifdef N2ENGINE_PHYSX_ENABLED
    if (options.physicsBackend == Config::ApplicationOptions::PhysicsBackend::PHYSX)
    {
        _3DphysicsBackend = std::make_unique<Physics::PhysXBackend>();
        if (!_3DphysicsBackend->Initialize())
        {
            Logger::Error("Failed to initialize PhysX backend!");
            Logger::Warn("Physics will be disabled. Game will continue without physics simulation.");
            _3DphysicsBackend.reset();
            physicsStatus.state = SubsystemState::Failed;
            physicsStatus.detail = "PhysX failed to initialize (see log)";
        }
        else
        {
            Logger::Info("3D Physics backend initialized successfully");
            physicsStatus.state = SubsystemState::Running;
            physicsStatus.detail = "PhysX";
        }
    }
    else
    {
        Logger::Error(NAMEOF(options.physicsBackend) + " is not currently supported");
        physicsStatus.state = SubsystemState::Disabled;
        physicsStatus.detail = "Selected physics backend is not supported";
    }
#else
    Logger::Info("Built without PhysX (N2ENGINE_USE_PHYSX=OFF); physics is disabled");
    physicsStatus.state = SubsystemState::Disabled;
    physicsStatus.detail = "Built without PhysX";
#endif

    if (options.isHeadless)
    {
        // Headless audio should be mixed and streamed to the editor client, not played locally
        Logger::Info("Audio disabled in headless mode (streaming not yet implemented)");
        audioStatus.state = SubsystemState::Disabled;
        audioStatus.detail = "Headless mode (streaming not yet implemented)";
    }
    else if (Audio::AudioSystem::Instance().Initialize())
    {
        audioStatus.state = SubsystemState::Running;
    }
    else
    {
        Logger::Error("Failed to initialize audio!");
        Logger::Warn("Audio will be disabled. Game will continue without sound.");
        audioStatus.state = SubsystemState::Failed;
        audioStatus.detail = "Could not open an audio device (see log)";
    }

    _health.subsystems = {windowStatus, rendererStatus, physicsStatus, audioStatus, scriptingStatus};
    for (const SubsystemStatus &status : _health.subsystems)
    {
        if (status.state == SubsystemState::Failed)
        {
            Logger::Warn(std::format("Subsystem {} failed: {}", status.name, status.detail));
        }
    }
    return _health;
}

/// @param initialScene The initial scene to load in SceneManager
/// @brief For testing only. An application should be configured and then the initial scene should be added later.
void Application::Init(std::unique_ptr<Scene> &&initialScene)
{
    Init();
    SceneManager::AddScene(std::move(initialScene), true);
    SceneManager::ProcessAnyPendingSceneChange();
    Logger::Info("Initial scene loaded: " + SceneManager::GetCurSceneRef().sceneName);
}

void Application::Run()
{
    if (!_window.IsValid())
    {
        Logger::Error("Cannot run: no window or renderer (see GetHealth())");
        Shutdown();
        return;
    }

    // Longest frame the fixed-step loop catches up on; after a long stall (debugger, loading) it would
    // otherwise queue hundreds of physics steps and fall further behind while running them
    constexpr double MaxFrameTime = 0.25;
    double fixedTimestepAccumulator = 0.0;

    while (!_window.ShouldClose() && !_quitRequested)
    {
        _window.PollEvents();

        Time::Update();
        // The frame delta itself, not a difference of float absolute times (which lost precision: ~1 ms
        // steps after 3 hours of running, 8 ms after 18)
        fixedTimestepAccumulator += std::min(static_cast<double>(Time::GetUnscaledDeltaTime()), MaxFrameTime);
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            Scene &curScene = SceneManager::GetCurSceneRef();
            curScene.ProcessAttachQueue();

            while (fixedTimestepAccumulator >= Time::GetFixedUnscaledDeltaTime())
            {
                // Paused (timeScale 0): no fixed steps, as in Unity; stepping PhysX by 0 is an error
                if (Time::GetFixedDeltaTime() > 0.0f)
                {
                    PhysicsUpdate(curScene);
                }
                fixedTimestepAccumulator -= Time::GetFixedUnscaledDeltaTime();
            }
            // OnMouse*: after the fixed steps (so it picks against this frame's physics) and before Update,
            // where Unity sends them
            _pointerDispatcher.Update();
            curScene.Update();
            curScene.AdvanceCoroutines();
            curScene.LateUpdate();
        }
        else
        {
            // Nothing to step. It used to keep accumulating, so the first scene to load ran a burst of
            // catch-up fixed steps.
            fixedTimestepAccumulator = 0.0;
        }
        // After LateUpdate, where listeners and sources push their positions
        Audio::AudioSystem::Instance().Update();
        Render();
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            Scene &curScene = SceneManager::GetCurSceneRef();
            curScene.ProcessDestroyed();
        }
        SceneManager::ProcessAnyPendingSceneChange();
    }

    // Window close and Quit() both end the run; components get OnApplicationQuit either way
    if (SceneManager::GetCurScene() != nullptr)
    {
        SceneManager::GetCurSceneRef().OnApplicationQuit();
    }
    Shutdown();
}

void Application::Shutdown()
{
    // Reverse of Init. Runs while function-local statics created during Init (e.g. the debug
    // console's log stream) are still alive; at process exit they're destroyed before this
    // singleton, so tearing subsystems down from ~Application would use them after destruction.
    // Scenes go first, while the audio sources and physics bodies their components hold can still be
    // released (left to SceneManager's static, they'd be freed after those subsystems were gone).
    SceneManager::UnloadScenes();
    Audio::AudioSystem::Instance().Shutdown();
    _3DphysicsBackend.reset(); // ~PhysXBackend releases the PhysX SDK
    _window.Shutdown();
    _initialized = false;
}

void Application::Render()
{
    auto *renderer = _window.GetRenderer();
    if (!renderer)
    {
        return;
    }

    _window.Clear();
    renderer->BeginFrame();

    if (SceneManager::GetCurSceneIndex() != -1)
    {
        auto &curScene = SceneManager::GetCurSceneRef();

        const Matrix4 &viewMatrix = _mainCamera->GetViewMatrix();
        const Matrix4 &projectionMatrix = _mainCamera->GetProjectionMatrix();
        renderer->SetViewProjection(viewMatrix.Data(), projectionMatrix.Data());
        const Renderer::Common::SceneLightingData sceneLightingData = curScene.CollectLighting();
        renderer->UpdateSceneLighting(sceneLightingData, _mainCamera->GetPosition());

        curScene.Render(renderer);
    }

    renderer->EndFrame();
    renderer->Present();
}

void Application::Quit()
{
    // A request, honoured at the end of the current frame: this used to std::exit(0) from wherever it
    // was called (usually inside a component or Lua callback, with scenes and the Lua stack live).
    // Run() then calls OnApplicationQuit and shuts down; hosts without Run() check IsQuitRequested().
    Logger::Info("Quit requested");
    GetInstance()._quitRequested = true;
}

void Application::OnWindowResize(const int width, const int height) const
{
    if (_mainCamera && width > 0 && height > 0)
    {
        // Calculate new aspect ratio
        const float newAspect = static_cast<float>(width) / static_cast<float>(height);

        // Update camera's aspect ratio - this will automatically trigger
        // projection matrix recalculation on next frame
        _mainCamera->UpdateAspectRatio(newAspect);

        // Logger::Info("Window resized to " + std::to_string(width) + "x" + std::to_string(height) + ", aspect ratio: " + std::to_string(newAspect));
    }
}

void Application::PhysicsUpdate(const Scene &scene) const
{
    if (_3DphysicsBackend)
    {
        _3DphysicsBackend->ApplyPendingChanges();
        scene.FixedUpdate();
        _3DphysicsBackend->Update(Time::GetFixedDeltaTime());

        // Sync physics results back to GameObjects
        _3DphysicsBackend->SyncTransforms();
        // notify collision events
        _3DphysicsBackend->ProcessCollisionCallbacks();
    }
    else
    {
        scene.FixedUpdate();
    }
}

Physics::IPhysicsBackend* Application::Get3DPhysicsBackend() const
{
    return _3DphysicsBackend.get();
}

void Application::Set3DPhysicsBackend(std::unique_ptr<Physics::IPhysicsBackend> backend)
{
    _3DphysicsBackend = std::move(backend);
}

void Application::RenderEditorFrame()
{
    if (!_window.IsValid())
    {
        return;
    }
    _window.PollEvents();
    Time::Update();
    Audio::AudioSystem::Instance().Update();
    Render();
}