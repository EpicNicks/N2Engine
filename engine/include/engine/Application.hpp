#pragma once
#include "engine/sceneManagement/SceneManager.hpp"

namespace N2Engine
{
    class Application
    {
        friend class SceneManager;

    private:
        Application();
        Application(const Application &) = delete;
        Application &operator=(const Application &) = delete;

    public:
        static Application &GetInstance();
        void Run();
    };
}