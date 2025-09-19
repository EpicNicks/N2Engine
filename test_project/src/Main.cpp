#include <engine/Application.hpp>
#include <engine/GameObject.hpp>
#include <engine/Positionable.hpp>
#include <engine/example/QuadRenderer.hpp>

#include <engine/GameObject.inl>

#include <iostream>

int main()
{
    std::cout << "Creating scene..." << std::endl;
    N2Engine::Scene testScene{"Test Scene"};

    auto quadObject = std::make_shared<N2Engine::GameObject>("TestQuad");
    auto quadRenderer = quadObject->AddComponent<N2Engine::Example::QuadRenderer>();
    quadRenderer->SetColor(N2Engine::Common::Color::Red());

    auto positionable = quadObject->GetPositionable();
    positionable->SetPosition(N2Engine::Math::Vector3{0.0f, 0.0f, 0.0f});
    positionable->SetScale(N2Engine::Math::Vector3{1.0f, 1.0f, 1.0f});
    positionable->SetRotation(N2Engine::Math::Quaternion::FromEulerAngles(0, 0, 45.0f));

    testScene.AddRootGameObject(quadObject);

    N2Engine::Application &application = N2Engine::Application::GetInstance();
    application.Init(testScene);

    application.Run();
    return 0;
}