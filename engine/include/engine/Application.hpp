#pragma once

#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

#include <string>
#include <memory>

namespace N2Engine
{
    class Application
    {
        friend class SceneManager;

    private:
        Window _window;
        std::unique_ptr<Camera> _mainCamera;
        std::unique_ptr<Physics::IPhysicsBackend> _3DphysicsBackend = nullptr;

    private:
        Application() = default;
        Application(const Application &) = delete;
        Application &operator=(const Application &) = delete;

        void Render();

    public:
        static Application &GetInstance();

        void Init();
        void Init(std::unique_ptr<Scene> &&initialScene);
        void Run();
        void PhysicsUpdate(Scene &scene);

        static void Quit();

        Camera *GetMainCamera();
        Window &GetWindow();

        void OnWindowResize(int width, int height);

        Physics::IPhysicsBackend *Get3DPhysicsBackend() const;
    };
}