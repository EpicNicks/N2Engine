#pragma once

#include <memory>

namespace N2Engine::Scripting
{
    struct ScriptCoroutineState;

    /// What LuaComponent:StartCoroutine returns to a script (Lua type ScriptCoroutine): a handle on one running script
    /// coroutine. Copies share it. The coroutine runs until its function returns, it is stopped, its object is
    /// deactivated or destroyed, or its component's script is replaced or switched off.
    class ScriptCoroutineHandle
    {
        std::shared_ptr<ScriptCoroutineState> _state;

    public:
        ScriptCoroutineHandle() = default;
        explicit ScriptCoroutineHandle(std::shared_ptr<ScriptCoroutineState> state) : _state(std::move(state)) {}

        /// True until the coroutine has finished, failed or been stopped
        [[nodiscard]] bool IsRunning() const;
        /// Stops it: no further part of its body runs. Does nothing once it is over.
        void Stop() const;
    };
}
