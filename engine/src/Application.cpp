#include <string>
#include <memory>

#ifdef N2ENGINE_DEBUG
#include <string_view>
#endif

#include <math/MathRegistrar.hpp>

#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/Logger.hpp"
#include "engine/example/QuadRenderer.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"

using namespace N2Engine;

Application &Application::GetInstance()
{
    static Application instance;
    return instance;
}

Window &Application::GetWindow()
{
    return _window;
}

Camera *Application::GetMainCamera() const
{
    return _mainCamera.get();
}

void Application::Init()
{
#ifdef N2ENGINE_DEBUG

#define RED(x) "\033[31m" x "\033[0m"
#define GREEN(x) "\033[32m" x "\033[0m"
#define YELLOW(x) "\033[33m" x "\033[0m"
#define BLUE(x) "\033[34m" x "\033[0m"

    auto originalStdout = Logger::RedirectStdout(Logger::LogLevel::Info, false);
    // auto originalStderr =
    Logger::RedirectStderr(Logger::LogLevel::Error, false);

    static auto originalStdoutStream = std::make_unique<std::ostream>(originalStdout);
    // static auto originalStderrStream = std::make_unique<std::ostream>(originalStderr);

    Logger::logEvent += [](const std::string_view msg, const Logger::LogLevel level)
    {
        const char *levelStr;
        switch (level)
        {
        case Logger::LogLevel::Info:
            levelStr = BLUE("INFO");
            break;
        case Logger::LogLevel::Warn:
            levelStr = YELLOW("WARN");
            break;
        case Logger::LogLevel::Error:
            levelStr = RED("ERROR");
            break;
        default:
            levelStr = BLUE("INFO");
            break;
        }
        *originalStdoutStream << "[" << levelStr << "] " << msg << std::endl;
    };
#endif

    Math::InitializeSIMD();
    Time::Init();
    _window.InitWindow();

    _mainCamera = std::make_unique<Camera>();

    const Vector2i windowDimensions = _window.GetWindowDimensions();
    const float aspect = static_cast<float>(windowDimensions[0]) / static_cast<float>(windowDimensions[1]);

    _mainCamera->SetPerspective(45.0f, aspect, 0.1f, 100.0f);
    // _mainCamera->SetOrthographic(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f);
    _mainCamera->SetPosition(Math::Vector3{0.0f, 0.0f, 10.0f});

    Logger::Info("Camera initialized");

    _3DphysicsBackend = std::make_unique<Physics::PhysXBackend>();
    if (!_3DphysicsBackend->Initialize())
    {
        Logger::Error("Failed to initialize PhysX backend!");
        Logger::Warn("Physics will be disabled. Game will continue without physics simulation.");
        _3DphysicsBackend.reset();
    }
    else
    {
        Logger::Info("3D Physics backend initialized successfully");
    }

    Logger::Info("3D Physics backend initialized");
}

void Application::Init(std::unique_ptr<Scene> &&initialScene)
{
    Init();
    SceneManager &instance = SceneManager::GetInstance();
    instance._scenes.push_back(std::move(initialScene));
    instance._curSceneIndex = 0;
    Logger::Info("Initial scene loaded: " + SceneManager::GetCurSceneRef().sceneName);
}

void Application::Run()
{
    double fixedTimestepAccumulator = 0.0;
    // Initialize last frame time for accumulator
    double lastTime = Time::GetUnscaledTime();

    while (!_window.ShouldClose())
    {
        _window.PollEvents();

        Time::Update();
        const double now = Time::GetUnscaledTime();
        const double frameTime = now - lastTime;
        lastTime = now;

        fixedTimestepAccumulator += frameTime;
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            Scene &curScene = SceneManager::GetCurSceneRef();
            curScene.ProcessAttachQueue();

            while (fixedTimestepAccumulator >= Time::GetFixedUnscaledDeltaTime())
            {
                PhysicsUpdate(curScene);
                fixedTimestepAccumulator -= Time::GetFixedUnscaledDeltaTime();
            }
            curScene.Update();
            curScene.AdvanceCoroutines();
            curScene.LateUpdate();
        }
        Render();
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            Scene &curScene = SceneManager::GetCurSceneRef();
            curScene.ProcessDestroyed();
        }
        SceneManager::ProcessAnyPendingSceneChange();
    }
}

void Application::Render()
{
    auto *renderer = _window.GetRenderer();

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
    if (SceneManager::GetCurSceneIndex() != -1)
    {
        const Scene &curScene = SceneManager::GetCurSceneRef();
        curScene.OnApplicationQuit();
    }
    std::exit(0);
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

Physics::IPhysicsBackend *Application::Get3DPhysicsBackend() const
{
    return _3DphysicsBackend.get();
}