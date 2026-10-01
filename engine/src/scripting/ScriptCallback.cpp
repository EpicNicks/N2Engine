#include "engine/scripting/ScriptCallback.hpp"

#include "engine/input/ActionMap.hpp"

namespace N2Engine::Scripting
{
    namespace
    {
        // Scripts only run on the main thread; thread_local keeps this correct if that ever changes
        thread_local std::shared_ptr<const bool> t_currentLifetime;
    }

    ScriptLifetimeScope::ScriptLifetimeScope(std::shared_ptr<const bool> lifetime)
        : _previous(std::move(t_currentLifetime))
    {
        t_currentLifetime = std::move(lifetime);
    }

    ScriptLifetimeScope::~ScriptLifetimeScope()
    {
        // Restore, so a script calling into another component's script and back is handled
        t_currentLifetime = std::move(_previous);
    }

    std::shared_ptr<const bool> ScriptLifetimeScope::Current()
    {
        return t_currentLifetime;
    }

    InputActionRef Detail::ToLuaArg(Input::InputAction &action)
    {
        return InputActionRef(action);
    }
}
