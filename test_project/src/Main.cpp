#include <engine/Application.hpp>

int main()
{
    N2Engine::Application &application = N2Engine::Application::GetInstance();
    application.Init();
    application.Run();
    return 0;
}