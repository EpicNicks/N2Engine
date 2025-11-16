#include <memory>

#include <math/Quaternion.hpp>
#include <math/Functions.hpp>
#include <engine/Application.hpp>
#include <engine/Time.hpp>
#include <engine/GameObject.hpp>
#include <engine/GameObject.inl>
#include <engine/Positionable.hpp>
#include <engine/example/QuadRenderer.hpp>

#include "test_project/Spin.hpp"

using namespace N2Engine;

Spin::Spin(N2Engine::GameObject &gameObject) : SerializableComponent(gameObject)
{
    RegisterMember(NAMEOF(degreesPerSecond), degreesPerSecond);
}

void Spin::OnAttach()
{
    auto positionable = _gameObject.GetPositionable();
    positionable->SetPosition(N2Engine::Math::Vector3{0.0f, 0.0f, 0.0f});
    positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0, 0, 0.0f));
    positionable->SetScale(N2Engine::Math::Vector3{3.0f, 3.0f, 1.0f});
}

void Spin::OnUpdate()
{
    // Logger::Info("delta time: " + std::to_string(Time::GetDeltaTime()));
    // Logger::Info("time: " + std::to_string(Time::GetTime()));
    auto mainCamera = Application::GetInstance().GetMainCamera();
    // mainCamera->SetPosition(_gameObject.GetPositionable()->GetPosition() + Math::Vector3{0.0f, 0.0f, 5.0f});

    static float totalTime = 0.0f;
    totalTime += Time::GetDeltaTime();

    // Rotate the quad around Y axis
    float angle = totalTime * degreesPerSecond; // 1 radian per second

    auto positionable = _gameObject.GetPositionable();
    if (positionable)
    {
        positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0.0f, angle, 0.0f));

        // Also try a slight wobble on position to make it more visible
        // float wobbleX = sin(totalTime * 2.0f) * 0.5f;
        // float wobbleY = cos(totalTime * 3.0f) * 0.3f;
        // positionable->SetPosition(Math::Vector3{wobbleX, wobbleY, 0.0f});
    }

    // yes GetComponent every frame is bad, but this is just a test
    if (auto quadComponent = _gameObject.GetComponent<N2Engine::Example::QuadRenderer>())
    {
        using namespace N2Engine::Math::Functions;

        float pingPongSpeed = 0.5f;
        float t = PingPong(Time::GetTime() * pingPongSpeed, 1.0f);
        N2Engine::Common::Color lerpColor = N2Engine::Common::Color::Lerp(N2Engine::Common::Color::Red(), N2Engine::Common::Color::Blue(), t);
        quadComponent->SetColor(N2Engine::Common::Color{lerpColor});
    }

    // mainCamera->SetPosition(mainCamera->GetPosition() + Math::Vector3{Time::GetDeltaTime(), Time::GetDeltaTime(), -Time::GetDeltaTime()});
}