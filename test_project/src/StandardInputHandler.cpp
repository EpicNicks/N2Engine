#include "test_project/StandardInputHandler.hpp"

#include <engine/Application.hpp>
#include <engine/GameObject.hpp>
#include <engine/Window.hpp>
#include <engine/input/Input.hpp>
#include <engine/input/ActionMap.hpp>

using namespace N2Engine;

StandardInputHandler::StandardInputHandler(GameObject &gameObject)
    : Component(gameObject)
{
}

StandardInputHandler::~StandardInputHandler() = default;

void StandardInputHandler::OnAttach()
{
    Application &application = Application::GetInstance();
    if (Input::ActionMap *actionMap = application.GetWindow().GetInputSystem()->LoadActionMap("Main Controls"); actionMap != nullptr)
    {
        (*actionMap)["Quit"].GetOnStateChanged() += [](const Input::InputAction &inputAction)
        {
            if (inputAction.GetPhase() == Input::ActionPhase::Started)
            {
                Application::Quit();
            }
        };
    }
}