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
    float yaw = 0.0f;
    float pitch = 0.0f;
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
        mainCamera->GetPosition() + mainCamera->GetRotation() * (Time::GetDeltaTime() * speed * Math::Vector3(
            pImpl->curInput.x, 0, -pImpl->curInput.y)));

    if (pImpl->curRotationInput.x != 0.0f || pImpl->curRotationInput.y != 0.0f)
    {
        constexpr float sensitivity = 2.0f;

        // Y input controls left/right (rotation around Y axis = engine's "pitch")
        pImpl->yaw += pImpl->curRotationInput.y * sensitivity * Time::GetDeltaTime();

        // X input controls up/down (rotation around X axis = engine's "roll")
        pImpl->pitch -= pImpl->curRotationInput.x * sensitivity * Time::GetDeltaTime();

        // Clamp pitch
        constexpr float maxPitch = 89.0f * Math::Constants::DEG_TO_RAD;
        pImpl->yaw = std::clamp(pImpl->yaw, -maxPitch, maxPitch);
    }

    // Constructor order is (pitch, yaw, roll) where:
    // - pitch = Y-axis rotation (our yaw/left-right)
    // - yaw = Z-axis rotation (keep at 0)
    // - roll = X-axis rotation (our pitch/up-down)
    const Math::Quaternion rotation(pImpl->pitch, 0.0f, pImpl->yaw);

    mainCamera->SetRotation(rotation);

    // const std::string curRotationInputString = "x: " + std::to_string(pImpl->curRotationInput.x) + ", y: " + std::to_string(pImpl->curRotationInput.y);
    // const Math::Vector3 eulerAngles = mainCamera->GetRotation().ToEulerAngles() * Math::Constants::RAD_TO_DEG;
    // const std::string rotationString = "x: " + std::to_string(eulerAngles.x) + ", y: " + std::to_string(eulerAngles.y) + ", z: " + std::to_string(eulerAngles.z);
    // Logger::Info("Camera rotation input: " + curRotationInputString + ", Camera rotation: " + rotationString);
}