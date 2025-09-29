#include "test_project/CameraController.hpp"

#include <engine/Application.hpp>
#include <engine/input/Input.hpp>

#include <memory>

#include <math/Vector3.hpp>
#include <math/Vector2.hpp>
#include <engine/Time.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;

CameraController::CameraController(N2Engine::GameObject &gameObject)
    : Component(gameObject), pImpl(std::make_unique<Impl>())
{
}

CameraController::~CameraController() = default;

struct CameraController::Impl
{
    N2Engine::Math::Vector2 curInput;
};

void CameraController::OnAttach()
{
    Application &application = Application::GetInstance();
    if (Input::ActionMap *actionMap = application.GetWindow().GetInputSystem()->LoadActionMap("Main Controls"); actionMap != nullptr)
    {
        Input::InputAction &cameraMoveInputAction = (*actionMap)["Camera Move"];
        cameraMoveInputAction.GetOnStateChanged() += [this](Input::InputAction &inputAction)
        {
            Logger::Info("CameraController logging phase for InputAction in callback: " + std::to_string((static_cast<int>(inputAction.GetPhase()))));
            this->pImpl->curInput = inputAction.GetVector2Value();
        };
    }
}

void CameraController::OnUpdate()
{
    auto &application = Application::GetInstance();
    auto mainCamera = application.GetMainCamera();
    float speed = 10.0f;
    mainCamera->SetPosition(mainCamera->GetPosition() + Time::GetDeltaTime() * speed * Math::Vector3(pImpl->curInput.x, pImpl->curInput.y, 0));
}