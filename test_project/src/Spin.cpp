#include <math/Quaternion.hpp>
#include <engine/Application.hpp>
#include <engine/Time.hpp>
#include <engine/GameObject.hpp>
#include <engine/Positionable.hpp>

#include "test_project/Spin.hpp"

using namespace N2Engine;

void Spin::OnUpdate()
{
    // Logger::Info("delta time: " + std::to_string(Time::GetDeltaTime()));
    // Logger::Info("time: " + std::to_string(Time::GetTime()));
    auto mainCamera = Application::GetInstance().GetMainCamera();
    // mainCamera->SetPosition(_gameObject.GetPositionable()->GetPosition() + Math::Vector3{0.0f, 0.0f, 5.0f});

    static float totalTime = 0.0f;
    totalTime += Time::GetDeltaTime();

    // Rotate the quad around Y axis
    float angle = totalTime * 1.0f; // 1 radian per second

    auto positionable = _gameObject.GetPositionable();
    if (positionable)
    {
        positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0.0f, angle, 0.0f));

        // Also try a slight wobble on position to make it more visible
        // float wobbleX = sin(totalTime * 2.0f) * 0.5f;
        // float wobbleY = cos(totalTime * 3.0f) * 0.3f;
        // positionable->SetPosition(Math::Vector3{wobbleX, wobbleY, 0.0f});
    }

    // mainCamera->SetPosition(mainCamera->GetPosition() + Math::Vector3{Time::GetDeltaTime(), Time::GetDeltaTime(), -Time::GetDeltaTime()});
}