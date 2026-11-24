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
        /// Stops it: no further part of its body runs, except the rest of the current step when called from inside its own body (it ends at the next yield). Does nothing once it is over.
        void Stop() const;
    };

    /// What WaitForSecondsRealtime(seconds) makes in Lua (issue #445): a script coroutine that yields it waits that many
    /// seconds of unscaled time (Scheduling::WaitForSecondsRealtime), so hit-stop (timeScale near 0) does not stretch it.
    /// The constructor checks the value (finite, >= 0), so a yielded one is always a valid wait.
    struct ScriptRealtimeWait
    {
        float seconds = 0.0f;
    };
}
