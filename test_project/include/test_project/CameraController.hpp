#pragma once

#include <memory>
#include <engine/Component.hpp>
#include <engine/common/ScriptUtils.hpp>

namespace N2Engine
{
    class GameObject;

    namespace Math
    {
        class Vector2;
    }
}

// Simple component, no serializable state
class CameraController : public N2Engine::Component
{
private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

public:
    CameraController(N2Engine::GameObject &gameObject);
    ~CameraController();
    void OnAttach() override;
    void OnUpdate() override;

    std::string GetTypeName() const override { return NAMEOF(CameraController); }
};