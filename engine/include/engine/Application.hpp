#pragma once

#include <memory>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Window.hpp"
#include "engine/Camera.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

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
        void Render();
        void PhysicsUpdate(const Scene &scene) const;

    public:
        Application(const Application &) = delete;
        Application& operator=(const Application &) = delete;
        static Application& GetInstance();

        void Init();
        void Init(const Config::ApplicationOptions &options);
        void Init(std::unique_ptr<Scene> &&initialScene);
        void Run();
        void RenderEditorFrame();

        static void Quit();

        [[nodiscard]] Camera* GetMainCamera() const;
        Window& GetWindow();

        void OnWindowResize(int width, int height) const;

        [[nodiscard]] Physics::IPhysicsBackend* Get3DPhysicsBackend() const;
    };
}
