#pragma once
#include "SceneManager.hpp"

class Application
{
    friend class SceneManager;

private:
    Application() {}
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

public:
    static Application &GetInstance();
    void Run();
};