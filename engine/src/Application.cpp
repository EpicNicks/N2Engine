#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/Logger.hpp"

#include <iostream>

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
    Logger::logEvent += [](const std::string &msg, Logger::LogLevel level)
    {
        const char *levelStr;
        switch (level)
        {
        case Logger::LogLevel::Info:
            levelStr = "INFO";
            break;
        case Logger::LogLevel::Warn:
            levelStr = "WARN";
            break;
        case Logger::LogLevel::Error:
            levelStr = "ERROR";
            break;
        default:
            levelStr = "INFO";
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

    _mainCamera->SetPerspective(45.0f, aspect, 0.1f, 100.0f);
    _mainCamera->SetPosition(Math::Vector3{0.0f, 0.0f, 3.0f});

    Logger::Log("Camera initialized", Logger::LogLevel::Info);
}

void Application::Run()
{

    // Main render loop
    while (!_window.ShouldClose())
    {
        _window.PollEvents();
        Time::Update();

        Render();
    }
}

void Application::Render()
{
    auto *renderer = _window.GetRenderer();

    renderer->BeginFrame();

    _window.Clear();
    const Matrix4 &viewMatrix = _mainCamera->GetViewMatrix();
    const Matrix4 &projectionMatrix = _mainCamera->GetProjectionMatrix();
    renderer->SetViewProjection(viewMatrix.data.data(), projectionMatrix.data.data());

    // TODO: Render scene objects here
    // This is where you'd iterate through your scene and render meshes

    renderer->EndFrame();
    renderer->Present();
}
