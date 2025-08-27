#include "Application.hpp"

Application &Application::GetInstance()
{
    static Application instance;
    return instance;
}

void Application::Run()
{
}

Application::Application()
{
}