#pragma once
#include "engine/sceneManagement/SceneManager.hpp"

namespace N2Engine
{
    enum class AppRenderer
    {
        Vulkan,
        OpenGL,
    };

    class Application
    {
        friend class SceneManager;

    private:
        Application() = default;
        Application(const Application &) = delete;
        Application &operator=(const Application &) = delete;

        AppRenderer ReadAppRendererFromConfig() const;

    public:
        static Application &GetInstance();
        void Init();
        void Run();
    };
}