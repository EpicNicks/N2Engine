#include <string>
#include <memory>
#include <algorithm>
#include <format>
#include <optional>
#include <vector>

#include <math/MathRegistrar.hpp>

#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/Logger.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/network/NetworkValidation.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/ui/UISystem.hpp"

using namespace N2Engine;

Application::Application()
{
    // UI first: an element under the pointer takes it, and the world pick doesn't run
    _pointerDispatcher.SetUIHitProvider(UI::UISystem::MakeApplicationHitProvider());
}

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

    // The project file is read once here: determinism.luaSeed must be set before the Lua state is created, math.simdTier
    // before anything else runs (and so before any scene loads), and physics.gpu is read from the same file below.
    // No project, or no setting: auto, as before.
    std::optional<IO::ProjectFile> project;
    if (!options.projectPath.empty())
    {
        if (auto loaded = IO::ProjectFile::Load(options.projectPath))
        {
            project = std::move(*loaded);
        }
    }

    // The network model, from the same project file. A bad value is reported by ApplyProjectSettings, like the others
    // here. The Deterministic preset is checked now, before anything is started: a project whose requirements are
    // unmet is refused (never silently degraded) with no window, no physics and no scene, so a host that checks
    // GetWindow().IsValid() stops. The physics backend's declarations are read from the type, because no backend
    // exists yet; none preset: nothing is checked.
    Network::NetworkSettings networkSettings;
    Network::NetworkEnvironment networkEnv;
    if (project)
    {
        networkSettings = ReadNetworkSettings(project->settings);
        networkEnv.gpuMode = ReadPhysicsGpuMode(project->settings);
        networkEnv.simdTier = ReadMathSimdTier(project->settings);
        networkEnv.luaSeedSet = ReadDeterminismLuaSeed(project->settings).has_value();
    }
    networkEnv.strictFp = Math::IsStrictFp();
#ifdef N2ENGINE_PHYSX_ENABLED
    if (options.physicsBackend == Config::ApplicationOptions::PhysicsBackend::PHYSX)
    {
        networkEnv.hasBackend = true;
        networkEnv.backendLevel = Physics::PhysXBackend::DeclaredDeterminism(
            project && ReadPhysicsDeterministic(project->settings));
        networkEnv.backendSupportsSnapshot = Physics::PhysXBackend::kSupportsSnapshot;
        networkEnv.backendSupportsSetState = Physics::PhysXBackend::kSupportsSetState;
    }
#endif
    Network::NetworkValidation networkValidation = Network::ValidateNetwork(networkSettings, networkEnv);
    if (networkValidation.Refused())
    {
        const auto unmet = networkValidation.Unmet();
        Logger::Error(std::format("network.preset is deterministic but {} requirement{} unmet: the engine refuses to "
                                  "start this project (it never silently degrades)",
                                  unmet.size(), unmet.size() == 1 ? " is" : "s are"));
        for (const Network::NetworkRequirement *requirement : unmet)
        {
            Logger::Error(std::format("  unmet: {} - {} ({})", requirement->id, requirement->text,
                                      requirement->detail));
        }
        // The usual rows, none started, then the Network row that says why
        for (const char *name : {"Window", "Renderer", "Physics", "Particles", "SoftBodies", "Audio", "Scripting"})
        {
            _health.subsystems.push_back({.name = name,
                                          .state = SubsystemState::NotStarted,
                                          .detail = "Not started: the network preset was refused (see Network)"});
        }
        _health.subsystems.push_back(Network::MakeNetworkStatus(networkSettings, networkValidation));
        _initialized = false;
        return _health;
    }

    // Before the first Instance(): the Lua state reads its seeds as it is created. Unset (also with no project) is
    // upstream Lua's behaviour. A bad value is reported by ApplyProjectSettings, as for physics.gpu.
    std::optional<std::uint64_t> luaSeed;
    if (project)
    {
        luaSeed = ReadDeterminismLuaSeed(project->settings);
    }
    Scripting::LuaRuntime::SetLuaSeed(luaSeed);
    if (luaSeed)
    {
        Logger::Warn("determinism.luaSeed is set: Lua's string hash seed is a constant, so keys an attacker controls "
                     "can be chosen to collide (hash flooding). Leave it unset unless the scripts' data is trusted.");
    }

    if (Scripting::LuaRuntime::Instance().Initialize())
    {
        scriptingStatus.state = SubsystemState::Running;
    }
    else
    {
        scriptingStatus.state = SubsystemState::Failed;
        scriptingStatus.detail = "Lua bindings failed to register (see log)";
    }

    // A bad value is reported by ApplyProjectSettings (the hosts call it after Init), as for physics.gpu, so it is
    // warned once, not twice
    Math::SIMDTierRequest simdRequest = Math::SIMDTierRequest::Auto;
    if (project)
    {
        simdRequest = ReadMathSimdTier(project->settings);
    }
    const Math::SIMDLevel simdLevel = Math::InitializeSIMD(simdRequest);
    if (const auto wanted = Math::ToSIMDLevel(simdRequest); wanted && *wanted != simdLevel)
    {
        Logger::Warn(std::format("math.simdTier {} is not supported on this CPU/architecture; using {} instead",
                                 Math::SIMDLevelName(*wanted), Math::SIMDLevelName(simdLevel)));
    }
    Logger::Info(std::format("Math SIMD tier: {} ({})", Math::SIMDLevelName(simdLevel),
                             simdRequest == Math::SIMDTierRequest::Auto ? "auto-detected" : "forced by math.simdTier"));
    Time::Init();
    // A project's rendering settings belong to that project: a second Init in the same process starts from the defaults
    Rendering::RenderSettings::SetColorSpace(Rendering::ColorSpace::Gamma);
    Rendering::RenderSettings::SetForceShaderEncode(false);

    const std::string rendererName{RenderBackendName(options.renderBackend)};
    if (_window.InitWindow(options) && _window.IsWindowless())
    {
        // Deliberately no window (Window::UsesNoWindow): healthy, and frames are read back instead
        windowStatus.state = SubsystemState::Disabled;
        windowStatus.detail = "No window: the headless software renderer draws offscreen";
        rendererStatus.state = SubsystemState::Running;
        rendererStatus.detail = rendererName + " (headless)";
    }
    else if (_window.IsValid())
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

    // physics.gpu comes from the project file here, not from ApplyProjectSettings (which runs after Init): the
    // physics scene is built below and its GPU flag cannot change afterwards. No project, or no setting: off, and
    // then no CUDA call is made at all. A bad value is reported by ApplyProjectSettings.
    Physics::GpuMode gpuMode = Physics::GpuMode::Off;
    bool deterministicPhysics = false; // physics.deterministic, read here for the same reason
    if (project)
    {
        gpuMode = ReadPhysicsGpuMode(project->settings);
        deterministicPhysics = ReadPhysicsDeterministic(project->settings);
    }

#ifdef N2ENGINE_PHYSX_ENABLED
    if (options.physicsBackend == Config::ApplicationOptions::PhysicsBackend::PHYSX)
    {
        _3DphysicsBackend = std::make_unique<Physics::PhysXBackend>();
        // Before Initialize: the scene's GPU flag is fixed when it is created
        _3DphysicsBackend->SetGpuMode(gpuMode);
        _3DphysicsBackend->SetDeterministic(deterministicPhysics);
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

#ifdef N2ENGINE_PLATFORM_WEB
    // The web player runs "headless" (no window: the page shows the frames), but its sound is meant to be heard, so it
    // opens the browser's output (Emscripten's OpenAL on WebAudio) like a game with a window. The editor host, the only
    // user of the loopback stream, is not built for the web. With N2ENGINE_WEB_AUDIO=stub no device opens either, so
    // both web configurations end in the one "Subsystem Audio failed" warning. docs/web-build.html
    constexpr bool audioToLoopback = false;
#else
    const bool audioToLoopback = options.isHeadless;
#endif
    if (audioToLoopback)
    {
        // Headless audio is mixed into a loopback device (nothing plays locally) and streamed to the editor
        // client; see AudioSystem::AdvanceStream and the editor protocol's GetAudio
        auto &audio = Audio::AudioSystem::Instance();
        if (audio.Initialize(Audio::AudioOutput::Loopback))
        {
            const Audio::LoopbackFormat &format = audio.GetLoopbackFormat();
            audioStatus.state = SubsystemState::Running;
            audioStatus.detail = std::format("Loopback {} Hz, {} channels, {} (streamed to the editor client)",
                                             format.sampleRate, format.channels,
                                             Audio::ToString(format.sampleFormat));
        }
        else
        {
            Logger::Warn("Headless audio is disabled: no OpenAL loopback device. The engine continues without sound.");
            audioStatus.state = SubsystemState::Failed;
            audioStatus.detail = "No OpenAL loopback device (ALC_SOFT_loopback; see log)";
        }
    }
    else if (Audio::AudioSystem::Instance().Initialize())
    {
        audioStatus.state = SubsystemState::Running;
    }
    else
    {
#ifdef N2ENGINE_PLATFORM_WEB
        // One warning, the "Subsystem Audio failed" line below: no WebAudio (a browser without it, a blocked or failed
        // AudioContext, node), exactly like a machine with no sound card
        audioStatus.state = SubsystemState::Failed;
        audioStatus.detail = "No WebAudio output: audio is off, the game continues without sound";
#else
        Logger::Error("Failed to initialize audio!");
        Logger::Warn("Audio will be disabled. Game will continue without sound.");
        audioStatus.state = SubsystemState::Failed;
        audioStatus.detail = "Could not open an audio device (see log)";
#endif
    }

    // The Network row. The Authoritative preset only warns, and does so against the backend that exists now (a
    // Deterministic project never gets here: it was refused above)
    if (networkSettings.preset == Network::NetworkPreset::Authoritative)
    {
        networkEnv.hasBackend = _3DphysicsBackend != nullptr;
        networkEnv.backendLevel = _3DphysicsBackend ? _3DphysicsBackend->GetDeterminism()
                                                    : Physics::DeterminismLevel::None;
        networkEnv.backendSupportsSnapshot = _3DphysicsBackend && _3DphysicsBackend->SupportsSnapshot();
        networkEnv.backendSupportsSetState = _3DphysicsBackend && _3DphysicsBackend->SupportsSetState();
        networkValidation = Network::ValidateNetwork(networkSettings, networkEnv);
        for (const std::string &warning : networkValidation.warnings)
        {
            Logger::Warn("network.preset is authoritative: " + warning);
        }
    }
    const SubsystemStatus networkStatus = Network::MakeNetworkStatus(networkSettings, networkValidation);

    // The Particles and SoftBodies rows follow the physics GPU probe: Disabled with the reason unless the GPU scene
    // is up (and, for each, its own feature has not failed)
    const Physics::GpuStatus noBackendGpu{gpuMode == Physics::GpuMode::Off ? Physics::GpuState::Disabled
                                                                           : Physics::GpuState::Unavailable,
                                          gpuMode == Physics::GpuMode::Off
                                              ? "physics.gpu is off"
                                              : "GPU physics unavailable: there is no physics backend (see Physics)"};
    const SubsystemStatus particlesStatus = Physics::MakeParticlesStatus(
        _3DphysicsBackend ? _3DphysicsBackend->GetParticlesGpuStatus() : noBackendGpu);
    const SubsystemStatus softBodiesStatus = Physics::MakeSoftBodiesStatus(
        _3DphysicsBackend ? _3DphysicsBackend->GetSoftBodiesGpuStatus() : noBackendGpu);

    _health.subsystems = {windowStatus,    rendererStatus, physicsStatus, particlesStatus,
                          softBodiesStatus, audioStatus,   scriptingStatus, networkStatus};
    _particlesBaseDetail = particlesStatus.detail;
    _softBodiesBaseDetail = softBodiesStatus.detail;
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
    if (!BeginRun())
    {
        return;
    }
    while (RunFrame())
    {
    }
    EndRun();
}

bool Application::BeginRun()
{
    if (!_window.IsValid())
    {
        Logger::Error("Cannot run: no window or renderer (see GetHealth())");
        Shutdown();
        return false;
    }

    _fixedTimestepAccumulator = 0.0;
    return true;
}

bool Application::RunFrame()
{
    if (_window.ShouldClose() || _quitRequested)
    {
        return false;
    }
    Tick();
    return true;
}

void Application::EndRun()
{
    // Window close and Quit() both end the run; components get OnApplicationQuit either way
    if (SceneManager::GetCurScene() != nullptr)
    {
        SceneManager::GetCurSceneRef().OnApplicationQuit();
    }
    Shutdown();
}

void Application::Tick(const TickOptions &options)
{
    // Longest frame the fixed-step loop catches up on; after a long stall (debugger, loading) it would
    // otherwise queue hundreds of physics steps and fall further behind while running them
    constexpr double MaxFrameTime = 0.25;

    if (options.pollEvents)
    {
        _window.PollEvents();
    }

    if (options.deltaSeconds)
    {
        Time::Advance(*options.deltaSeconds);
    }
    else
    {
        Time::Update();
    }
    // The frame delta itself, not a difference of float absolute times (which lost precision: ~1 ms
    // steps after 3 hours of running, 8 ms after 18)
    _fixedTimestepAccumulator += std::min(static_cast<double>(Time::GetUnscaledDeltaTime()), MaxFrameTime);
    if (SceneManager::GetCurSceneIndex() != -1)
    {
        Scene &curScene = SceneManager::GetCurSceneRef();
        curScene.ProcessAttachQueue();

        while (_fixedTimestepAccumulator >= Time::GetFixedUnscaledDeltaTime())
        {
            // Paused (timeScale 0): no fixed steps, as in Unity; stepping PhysX by 0 is an error
            if (Time::GetFixedDeltaTime() > 0.0f)
            {
                PhysicsUpdate(curScene);
            }
            _fixedTimestepAccumulator -= Time::GetFixedUnscaledDeltaTime();
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
        _fixedTimestepAccumulator = 0.0;
    }
    // After LateUpdate, where listeners and sources push their positions. A loopback device (headless) only
    // mixes when asked, so it is paced here by real elapsed time (no-op with a sound card), then Update
    // recycles whatever that finished.
    Audio::AudioSystem::Instance().AdvanceStream(Time::GetUnscaledDeltaTime());
    Audio::AudioSystem::Instance().Update();
    if (options.render)
    {
        Render();
    }
    if (SceneManager::GetCurSceneIndex() != -1)
    {
        Scene &curScene = SceneManager::GetCurSceneRef();
        curScene.ProcessDestroyed();
    }
    SceneManager::ProcessAnyPendingSceneChange();
}

void Application::ResetFrameClock()
{
    Time::ResetFrameClock();
}

void Application::RenderGameFrame()
{
    if (!_window.IsValid())
    {
        return;
    }
    Render();
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
    // The main camera exists after Init; before it (a host driving a bare window) a default one stands in
    static const Camera fallbackCamera;
    Render(_mainCamera ? *_mainCamera : fallbackCamera);
}

void Application::Render(const Camera &camera, const bool screenSpaceUI)
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

        const Matrix4 &viewMatrix = camera.GetViewMatrix();
        const Matrix4 &projectionMatrix = camera.GetProjectionMatrix();
        renderer->SetViewProjection(viewMatrix.Data(), projectionMatrix.Data());
        const Renderer::Common::SceneLightingData sceneLightingData = curScene.CollectLighting();
        renderer->UpdateSceneLighting(sceneLightingData, camera.GetPosition());

        curScene.Render(renderer, camera);
        // The UI pass, over the scene: canvases in sort order, in window coordinates (the cursor's space), or
        // over the frame's own size when Window::SetRenderSize set one (the editor's viewport)
        if (screenSpaceUI)
        {
            UI::UISystem::Render(curScene, renderer, _window.GetRenderDimensions());
        }
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

void Application::SetScriptQuitAllowed(const bool allowed)
{
    _scriptQuitAllowed = allowed;
    _scriptQuitWarned = false;
}

bool Application::IsScriptQuitAllowed()
{
    return _scriptQuitAllowed;
}

void Application::QuitFromScript()
{
    if (_scriptQuitAllowed)
    {
        Quit();
        return;
    }
    if (!_scriptQuitWarned)
    {
        _scriptQuitWarned = true;
        Logger::Warn("Application.Quit is ignored on an editor host");
    }
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

const EngineHealth& Application::GetHealth() const
{
    // Each row's state is the physics backend's status for that feature as it is now (Disabled, Running, Failed):
    // a GPU that stopped working for particles after Init (the PhysX backend gave up on its GPU particle backend)
    // shows on the Particles row and only there; the SoftBodies row has its own failure state.
    // The Particles detail names the backends the live ParticleSystems use; without any it is the status's own detail.
    if (auto it = std::ranges::find(_health.subsystems, std::string_view("Particles"), &SubsystemStatus::name);
        it != _health.subsystems.end())
    {
        std::string baseDetail = _particlesBaseDetail;
        if (_3DphysicsBackend)
        {
            const SubsystemStatus current = Physics::MakeParticlesStatus(_3DphysicsBackend->GetParticlesGpuStatus());
            it->state = current.state;
            baseDetail = current.detail;
        }
        it->detail = Particles::ParticleUsage::Compose(baseDetail);
    }
    // The SoftBodies detail names the backends the live SoftBody components use (without any it is the status's own
    // detail, so the row is what Init made), also with no physics backend (every body then runs on the CPU).
    if (auto it = std::ranges::find(_health.subsystems, std::string_view("SoftBodies"), &SubsystemStatus::name);
        it != _health.subsystems.end())
    {
        std::string baseDetail = _softBodiesBaseDetail;
        if (_3DphysicsBackend)
        {
            const SubsystemStatus current = Physics::MakeSoftBodiesStatus(_3DphysicsBackend->GetSoftBodiesGpuStatus());
            it->state = current.state;
            baseDetail = current.detail;
        }
        it->detail = SoftBodies::SoftBodyUsage::Compose(baseDetail);
    }
    return _health;
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

void Application::RenderEditorFrame(const Camera &camera)
{
    if (!_window.IsValid())
    {
        return;
    }
    // A scene view is not the game: no window events or input (the host's client drives it), and no screen-space UI
    Time::Update();
    Audio::AudioSystem::Instance().Update();
    Render(camera, false);
}