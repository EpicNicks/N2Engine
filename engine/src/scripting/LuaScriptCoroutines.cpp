// Script coroutines: LuaComponent:StartCoroutine(fn, ...) runs fn in a Lua coroutine that the GameObject's coroutine
// scheduler resumes (one C++ generator per Lua coroutine). What the function yields says how long to wait:
//   coroutine.yield()      the next frame (nil, or nothing)
//   coroutine.yield(2.5)   2.5 seconds of game time (a number >= 0; 0 is the next frame)
// Any other yielded value is an error that ends the coroutine. The first resume is on the first scheduler update after
// StartCoroutine (nothing of the body runs in StartCoroutine itself), as for C++ coroutines.
//
// Every resume is one entry into Lua like a lifecycle call: a LuaBudgetScope, the owning component's lifetime scope,
// and the coroutine's thread under the entry's count hook, charged after the run (LuaRuntime.cpp, PrepareThreadHook and
// ChargeThread). An error is logged as a component's script error; a budget or memory error switches the component off.

#include <algorithm>
#include <cmath>
#include <limits>
#include <format>
#include <generator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "LuaGuard.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/scheduling/CoroutineWait.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/scripting/ScriptCoroutine.hpp"

namespace N2Engine::Scripting
{
    struct ScriptCoroutineState
    {
        lua_State *main = nullptr;   // the runtime's state (the registry is shared by all its threads)
        lua_State *thread = nullptr; // the coroutine, kept alive by `ref`
        int ref = LUA_NOREF;
        int pendingArguments = 0;    // the function's arguments, on the thread's stack until the first resume
        bool stopped = false;
        bool finished = false;
        LuaComponent *owner = nullptr; // valid only while *lifetime
        std::shared_ptr<const bool> lifetime;
        Scheduling::Coroutine *handle = nullptr;

        ~ScriptCoroutineState() { ReleaseThread(); }

        // Frees the Lua thread (its stack and what it references) as soon as the coroutine is over
        void ReleaseThread()
        {
            if (ref != LUA_NOREF && main != nullptr && LuaRuntime::HasInstance())
            {
                luaL_unref(main, LUA_REGISTRYINDEX, ref);
            }
            ref = LUA_NOREF;
            thread = nullptr;
        }

        // Ends the state: the scheduler can destroy the generator without running any of our code (the object is
        // deactivated or destroyed), so this also runs from the generator frame's destructor
        void Finish()
        {
            finished = true;
            handle = nullptr;
            ReleaseThread();
        }

        [[nodiscard]] bool OwnerAlive() const { return lifetime && *lifetime && owner != nullptr; }
        [[nodiscard]] bool IsOver() const { return stopped || finished; }
    };

    namespace
    {
        struct Step
        {
            bool done = true;
            float seconds = 0.0f; // the wait when not done; 0 is the next frame
        };

        // One resume. Never throws, and never leaves the state half-run: the thread's status decides.
        Step ResumeOnce(ScriptCoroutineState &state)
        {
            if (state.IsOver() || !state.OwnerAlive() || state.owner->IsFaulted() || !LuaRuntime::RunsScripts())
            {
                return {};
            }
            LuaComponent *owner = state.owner;
            const LuaBudgetScope budget;
            ScriptLifetimeScope scope(state.lifetime);

            lua_State *thread = state.thread;
            Detail::PrepareThreadHook(thread);
            int results = 0;
            const int status = lua_resume(thread, state.main, state.pendingArguments, &results);
            state.pendingArguments = 0;
            if (state.stopped)
            {
                lua_settop(thread, 0);
                return {}; // stopped from inside its own body: it ends here, at this yield or return
            }
            const bool spent = Detail::ChargeThread(thread);

            if (spent)
            {
                // The budget ran out in the coroutine (or before it): the error of any entry that does
                owner->ReportCoroutineError(LuaRuntime::Instance().LimitAwareError({}));
                return {};
            }
            if (status == LUA_OK)
            {
                lua_settop(thread, 0);
                return {}; // the function returned
            }
            if (status != LUA_YIELD)
            {
                const int type = lua_type(thread, -1);
                std::string text;
                if (type == LUA_TSTRING || type == LUA_TNUMBER)
                {
                    text = lua_tostring(thread, -1);
                }
                else
                {
                    text = std::format("(error object is a {} value)", luaL_typename(thread, -1));
                }
                const std::string message = LuaRuntime::Instance().LimitAwareError(text, status == LUA_ERRMEM);
                lua_settop(thread, 0);
                owner->ReportCoroutineError(message);
                return {};
            }

            // Yielded: what it yielded says how long to wait
            Step step;
            step.done = false;
            if (results > 0 && lua_type(thread, -results) != LUA_TNIL)
            {
                if (lua_type(thread, -results) == LUA_TNUMBER)
                {
                    const double seconds = lua_tonumber(thread, -results);
                    if (std::isfinite(seconds) && seconds >= 0.0)
                    {
                        step.seconds = static_cast<float>(std::min(seconds, static_cast<double>(std::numeric_limits<float>::max())));
                    }
                    else
                    {
                        step.done = true;
                        owner->ReportCoroutineError("coroutine.yield: the wait must be a finite number of seconds >= 0");
                    }
                }
                else
                {
                    step.done = true;
                    owner->ReportCoroutineError(std::format(
                        "coroutine.yield: expected nothing or a number of seconds, got a {}", luaL_typename(thread, -results)));
                }
            }
            lua_settop(thread, 0);
            return step;
        }

        struct FinishGuard
        {
            std::shared_ptr<ScriptCoroutineState> state;
            ~FinishGuard() { state->Finish(); }
        };

        std::generator<Scheduling::ICoroutineWait> RunScriptCoroutine(std::shared_ptr<ScriptCoroutineState> state)
        {
            const FinishGuard guard{state};
            while (true)
            {
                const Step step = ResumeOnce(*state);
                if (step.done)
                {
                    co_return;
                }
                if (step.seconds > 0.0f)
                {
                    co_yield Scheduling::WaitForSeconds{step.seconds};
                }
                else
                {
                    co_yield Scheduling::WaitForNextFrame{};
                }
                if (state->IsOver() || !state->OwnerAlive())
                {
                    co_return;
                }
            }
        }

        void StopState(ScriptCoroutineState &state)
        {
            if (state.IsOver())
            {
                return;
            }
            state.stopped = true;
            if (state.OwnerAlive() && state.handle != nullptr)
            {
                // Removes it from the scheduler (deferred to the end of the update when one is running)
                state.owner->GetGameObject().StopCoroutine(state.handle);
            }
        }
    }

    bool ScriptCoroutineHandle::IsRunning() const
    {
        return _state != nullptr && !_state->IsOver();
    }

    void ScriptCoroutineHandle::Stop() const
    {
        if (_state != nullptr)
        {
            StopState(*_state);
        }
    }

    void LuaComponent::TrackScriptCoroutine(const std::shared_ptr<ScriptCoroutineState> &state)
    {
        // Pruned when the list has doubled since the last prune, so starting k coroutines costs O(k) in all
        if (_coroutines.size() >= _coroutinesPruneAt)
        {
            std::erase_if(_coroutines, [](const std::weak_ptr<ScriptCoroutineState> &w) { return w.expired(); });
            _coroutinesPruneAt = std::max<std::size_t>(16, _coroutines.size() * 2);
        }
        _coroutines.push_back(state);
    }

    void LuaComponent::StopAllScriptCoroutines()
    {
        const auto coroutines = std::move(_coroutines);
        _coroutines.clear();
        for (const auto &weak : coroutines)
        {
            if (const auto state = weak.lock())
            {
                StopState(*state);
            }
        }
    }

    ScriptCoroutineHandle LuaComponent::StartScriptCoroutine(lua_State *L, const int firstIndex, const int count)
    {
        if (count < 1 || lua_type(L, firstIndex) != LUA_TFUNCTION)
        {
            throw std::runtime_error("LuaComponent:StartCoroutine: the first argument must be a function");
        }
        if (!LuaRuntime::RunsScripts() || !_scriptInstance.valid() || _faulted)
        {
            return {};
        }

        std::string error;
        if (!LuaRuntime::Instance().ReserveReferences(2, error))
        {
            throw std::runtime_error("LuaComponent:StartCoroutine: " + error);
        }

        if (lua_checkstack(L, count + 2) == 0)
        {
            throw std::runtime_error("LuaComponent:StartCoroutine: too many arguments");
        }
        auto state = std::make_shared<ScriptCoroutineState>();
        state->main = LuaRuntime::Instance().GetState().lua_state();
        state->owner = this;
        state->lifetime = _alive;
        lua_State *thread = lua_newthread(L); // a memory error here is a Lua error: the binding's caller reports it
        state->ref = luaL_ref(L, LUA_REGISTRYINDEX); // takes the thread off L's stack
        state->thread = thread;
        if (lua_checkstack(thread, count + 2) == 0)
        {
            throw std::runtime_error("LuaComponent:StartCoroutine: too many arguments"); // the state frees the thread
        }
        for (int i = 0; i < count; ++i)
        {
            lua_pushvalue(L, firstIndex + i);
        }
        lua_xmove(L, thread, count);
        state->pendingArguments = count - 1; // the function itself is not an argument

        Scheduling::Coroutine *coroutine = GetGameObject().StartCoroutine(RunScriptCoroutine(state));
        if (coroutine == nullptr)
        {
            return {}; // an inactive object, or not in a scene (logged by GameObject::StartCoroutine); the state frees the thread
        }
        state->handle = coroutine;
        TrackScriptCoroutine(state);
        return ScriptCoroutineHandle(std::move(state));
    }
}
