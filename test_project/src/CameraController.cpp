#include <memory>

#include <math/Vector3.hpp>
#include <math/Vector2.hpp>
#include <math/Quaternion.hpp>
#include <math/Constants.hpp>
#include <engine/Time.hpp>
#include <engine/Application.hpp>
#include <engine/input/Input.hpp>

#include "test_project/CameraController.hpp"

using namespace N2Engine;

struct CameraController::Impl
{
    Math::Vector2 curInput;
    Math::Vector2 curRotationInput;
    float pitch = 0.0f; // X rotation - look up/down
    float yaw = 0.0f; // Y rotation - turn left/right
};

CameraController::CameraController(GameObject& gameObject)
    : Component(gameObject), pImpl(std::make_unique<Impl>())
{
}

void CameraController::OnAttach()
{
    Application& application = Application::GetInstance();
    if (auto* actionMap = application.GetWindow().GetInputSystem()->LoadActionMap("Main Controls"))
    {
        (*actionMap)["Camera Move"].GetOnStateChanged() += [this](Input::InputAction& action)
        {
            pImpl->curInput = action.GetVector2Value();
        };
        (*actionMap)["Camera Rotate"].GetOnStateChanged() += [this](Input::InputAction& action)
        {
            pImpl->curRotationInput = action.GetVector2Value();
        };
    }
}

void CameraController::OnUpdate()
{
    auto *mainCamera = Application::GetInstance().GetMainCamera();
    if (!mainCamera)
        return;

    // Movement
    constexpr float speed = 10.0f;
    const Math::Vector3 moveDir(pImpl->curInput.x, 0.0f, -pImpl->curInput.y);
    mainCamera->SetPosition(
        mainCamera->GetPosition() + mainCamera->GetRotation() * (moveDir * speed * Time::GetDeltaTime())
    );

    // Rotation
    if (pImpl->curRotationInput.x != 0.0f || pImpl->curRotationInput.y != 0.0f)
    {
        constexpr float sensitivity = 2.0f;
        const float deltaTime = Time::GetDeltaTime();

        // Turn left/right
        pImpl->yaw -= pImpl->curRotationInput.x * sensitivity * deltaTime;

        // Look up/down
        pImpl->pitch += pImpl->curRotationInput.y * sensitivity * deltaTime;

        constexpr float maxPitch = 89.0f * Math::Constants::DEG_TO_RAD;
        pImpl->pitch = std::clamp(pImpl->pitch, -maxPitch, maxPitch);
    }

    const Math::Quaternion rotation = Math::Quaternion::FromEulerAngles(
        pImpl->pitch,
        pImpl->yaw,
        0.0f
    );

    mainCamera->SetRotation(rotation);
}
