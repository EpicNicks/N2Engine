#pragma once

#include <engine/Component.hpp>
#include <engine/common/ScriptUtils.hpp>

namespace N2Engine
{
    class GameObject;
}

class StandardInputHandler : public N2Engine::Component
{
public:
    StandardInputHandler(N2Engine::GameObject &gameObject);
    ~StandardInputHandler();
    void OnAttach() override;
    std::string GetTypeName() const override { return NAMEOF(StandardInputHandler); }
};