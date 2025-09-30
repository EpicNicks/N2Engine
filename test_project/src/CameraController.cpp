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
    N2Engine::Math::Vector2 curRotationInput;
};

void CameraController::OnAttach()
{
    Application &application = Application::GetInstance();
    if (Input::ActionMap *actionMap = application.GetWindow().GetInputSystem()->LoadActionMap("Main Controls"); actionMap != nullptr)
    {
        (*actionMap)["Camera Move"].GetOnStateChanged() += [&](Input::InputAction &inputAction)
        {
            // Logger::Info("CameraController logging phase for InputAction in callback: " + std::to_string((static_cast<int>(inputAction.GetPhase()))));
            this->pImpl->curInput = inputAction.GetVector2Value();
        };
        (*actionMap)["Camera Rotate"].GetOnStateChanged() += [&](Input::InputAction &inputAction)
        {
            this->pImpl->curRotationInput = inputAction.GetVector2Value();
        };
    }
}

void CameraController::OnUpdate()
{
    auto &application = Application::GetInstance();
    auto mainCamera = application.GetMainCamera();
    float speed = 10.0f;
    mainCamera->SetPosition(mainCamera->GetPosition() + Time::GetDeltaTime() * speed * Math::Vector3(pImpl->curInput.x, pImpl->curInput.y, 0));

    if (pImpl->curRotationInput.x != 0.0f || pImpl->curRotationInput.y != 0.0f)
    {
        float rotationSpeed = 90.0f;
        float deltaTime = Time::GetDeltaTime();

        float degreesToRadians = std::numbers::pi_v<float> / 180.0f;
        float yawDelta = pImpl->curRotationInput.x * rotationSpeed * deltaTime * degreesToRadians;
        float pitchDelta = -pImpl->curRotationInput.y * rotationSpeed * deltaTime * degreesToRadians;

        Math::Quaternion currentRotation = mainCamera->GetRotation();

        // Apply yaw rotation first (around world up)
        Math::Quaternion yawRotation = Math::Quaternion::FromAxisAngle(Math::Vector3::Up(), yawDelta);
        Math::Quaternion afterYaw = yawRotation * currentRotation;

        // Now get the local right vector AFTER yaw has been applied
        Math::Vector3 localRight = afterYaw * Math::Vector3::Right();
        Math::Quaternion pitchRotation = Math::Quaternion::FromAxisAngle(localRight, pitchDelta);

        // Apply pitch rotation
        Math::Quaternion newRotation = pitchRotation * afterYaw;
        newRotation.Normalize();

        mainCamera->SetRotation(newRotation);
    }
}