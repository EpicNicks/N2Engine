#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/Logger.hpp"

#include <engine/example/QuadRenderer.hpp>
#include "engine/sceneManagement/Scene.hpp"
#include "engine/GameObject.hpp"
#include <iostream>
#include <string>
#include <memory>

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

Camera *Application::GetMainCamera()
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

    Logger::logEvent += [](const std::string &msg, Logger::LogLevel level)
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
        std::cout << "[" << levelStr << "] " << msg << std::endl;
    };
#endif
    Time::Init();
    _window.InitWindow();

    _mainCamera = std::make_unique<Camera>();

    const Vector2i windowDimensions = _window.GetWindowDimensions();
    const float aspect = (float)windowDimensions[0] / (float)windowDimensions[1];

    //_mainCamera->SetPerspective(45.0f, aspect, 0.1f, 100.0f);
    _mainCamera->SetOrthographic(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f);
    _mainCamera->SetPosition(Math::Vector3{0.0f, 0.0f, 10.0f});

    Logger::Info("Camera initialized");
}

void Application::Init(const Scene &initialScene)
{
    Init();
    SceneManager::GetInstance()._scenes.push_back(initialScene);
    SceneManager::GetInstance()._curSceneIndex = 0;
    Logger::Info("Initial scene loaded: " + initialScene.sceneName);
}

void Application::Run()
{
    // Main render loop
    while (!_window.ShouldClose())
    {
        _window.PollEvents();
        Time::Update();
        if (SceneManager::GetCurSceneIndex() != -1)
        {
            Scene &curScene = SceneManager::GetCurScene();
            curScene.ProcessAttachQueue();
            curScene.Update();
        }
        Render();
    }
}

void Application::Render()
{
    auto *renderer = _window.GetRenderer();

    _window.Clear();
    renderer->BeginFrame();

    const Matrix4 &viewMatrix = _mainCamera->GetViewMatrix();
    const Matrix4 &projectionMatrix = _mainCamera->GetProjectionMatrix();

    renderer->SetViewProjection(viewMatrix.data.data(), projectionMatrix.data.data());

    if (SceneManager::GetCurSceneIndex() != -1)
    {
        SceneManager::GetCurScene().Render(renderer);
    }

    renderer->EndFrame();
    renderer->Present();
}
