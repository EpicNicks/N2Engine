#pragma once

#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"

#include <memory>

namespace N2Engine
{
    class Application
    {
        friend class SceneManager;

    private:
        Window _window;
        std::unique_ptr<Camera> _mainCamera;

    private:
        Application() = default;
        Application(const Application &) = delete;
        Application &operator=(const Application &) = delete;

        void Render();

    public:
        static Application &GetInstance();

        void Init();
        void Run();

        Camera *GetMainCamera();
        Window &GetWindow();
    };
}