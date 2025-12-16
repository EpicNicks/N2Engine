#include <memory>

#include <math/Vector3.hpp>
#include <math/Vector2.hpp>
#include <engine/Time.hpp>
#include <engine/Logger.hpp>
#include <math/Constants.hpp>

#include <engine/Application.hpp>
#include <engine/input/Input.hpp>

#include "test_project/CameraController.hpp"


using namespace N2Engine;

struct CameraController::Impl
{
    N2Engine::Math::Vector2 curInput;
    N2Engine::Math::Vector2 curRotationInput;
};

CameraController::CameraController(N2Engine::GameObject &gameObject)
    : Component(gameObject), pImpl(std::make_unique<Impl>()) {}

void CameraController::OnAttach()
{
    Application &application = Application::GetInstance();
    if (Input::ActionMap *actionMap = application.GetWindow().GetInputSystem()->LoadActionMap("Main Controls");
        actionMap != nullptr)
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
    const auto &application = Application::GetInstance();
    const auto mainCamera = application.GetMainCamera();
    constexpr float speed = 10.0f;
    mainCamera->SetPosition(
        mainCamera->GetPosition() + Time::GetDeltaTime() * speed * Math::Vector3(
            pImpl->curInput.x, pImpl->curInput.y, 0));

    const Math::Vector3 eulerAngles = mainCamera->GetRotation().ToEulerAngles() * Math::Constants::RAD_TO_DEG;
    const std::string rotationString = "x: " + std::to_string(eulerAngles.x) + ", y: " + std::to_string(eulerAngles.y) + ", z: " + std::to_string(eulerAngles.z);
    Logger::Info("Camera rotation: " + rotationString);

    if (pImpl->curRotationInput.x != 0.0f || pImpl->curRotationInput.y != 0.0f)
    {
        float rotationSpeed = 90.0f;
        float deltaTime = Time::GetDeltaTime();

        float degreesToRadians = std::numbers::pi_v<float> / 180.0f;
        float yawDelta = pImpl->curRotationInput.y * rotationSpeed * deltaTime * degreesToRadians;
        float pitchDelta = -pImpl->curRotationInput.x * rotationSpeed * deltaTime * degreesToRadians;

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
