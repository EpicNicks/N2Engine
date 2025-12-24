#include <memory>

#include <math/Quaternion.hpp>
#include <math/Functions.hpp>
#include <engine/Application.hpp>
#include <engine/Time.hpp>
#include <engine/GameObject.hpp>
#include <engine/GameObject.inl>
#include <engine/Positionable.hpp>
#include <../../engine/include/engine/example/renderers/QuadRenderer.hpp>

#include "test_project/Spin.hpp"

using namespace N2Engine;

Spin::Spin(GameObject &gameObject) : SerializableComponent(gameObject)
{
    RegisterMember(NAMEOF(degreesPerSecond), degreesPerSecond);
}

void Spin::OnAttach()
{
    auto *positionable = _gameObject.GetPositionable();
    positionable->SetPosition(N2Engine::Math::Vector3{0.0f, 0.0f, 0.0f});
    positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0, 0, 0.0f));
    positionable->SetScale(N2Engine::Math::Vector3{3.0f, 3.0f, 3.0f});
}

void Spin::OnUpdate()
{
    // Logger::Info("delta time: " + std::to_string(Time::GetDeltaTime()));
    // Logger::Info("time: " + std::to_string(Time::GetTime()));
    // auto mainCamera = Application::GetInstance().GetMainCamera();
    // mainCamera->SetPosition(_gameObject.GetPositionable()->GetPosition() + Math::Vector3{0.0f, 0.0f, 5.0f});

    static float totalTime = 0.0f;
    totalTime += Time::GetDeltaTime();

    // Rotate the quad around Y axis
    const float angle = totalTime * degreesPerSecond; // 1 radian per second

    if (const auto positionable = _gameObject.GetPositionable())
    {
        positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0.0f, angle, 0.0f));

        // Also try a slight wobble on position to make it more visible
        // float wobbleX = sin(totalTime * 2.0f) * 0.5f;
        // float wobbleY = cos(totalTime * 3.0f) * 0.3f;
        // positionable->SetPosition(Math::Vector3{wobbleX, wobbleY, 0.0f});
    }

    // yes GetComponent every frame is bad, but this is just a test
    if (const auto quadComponent = _gameObject.GetComponent<Example::QuadRenderer>())
    {
        constexpr float pingPongSpeed = 0.5f;
        const float t = N2Engine::Math::Functions::PingPong(Time::GetTime() * pingPongSpeed, 1.0f);
        const Common::Color lerpColor = Common::Color::Lerp(
            Common::Color::Red, Common::Color::Blue, t);
        quadComponent->SetColor(Common::Color{lerpColor});
    }

    // mainCamera->SetPosition(mainCamera->GetPosition() + Math::Vector3{Time::GetDeltaTime(), Time::GetDeltaTime(), -Time::GetDeltaTime()});
}
