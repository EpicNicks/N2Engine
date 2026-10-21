#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace N2Engine::N2Net
{
    template <class Signature>
    class FunctionRef;

    /// A non-owning reference to a callable, for the callbacks of the n2net API (Poll, packet sinks, message handlers).
    /// It never allocates and never throws, unlike std::function. The callable must outlive the call it is passed to:
    /// pass a lambda straight into the call, or keep a named lambda alive over it. Do not store a FunctionRef.
    template <class R, class... Args>
    class FunctionRef<R(Args...)>
    {
    public:
        template <class F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_invocable_r_v<R, F &, Args...>)
        FunctionRef(F &&callable) noexcept
            : object_(const_cast<void *>(static_cast<const void *>(std::addressof(callable)))),
              call_(&Invoke<std::remove_reference_t<F>>)
        {
        }

        R operator()(Args... args) const
        {
            return call_(object_, std::forward<Args>(args)...);
        }

    private:
        template <class F>
        static R Invoke(void *object, Args... args)
        {
            return (*static_cast<F *>(object))(std::forward<Args>(args)...);
        }

        void *object_;
        R (*call_)(void *, Args...);
    };
} // namespace N2Engine::N2Net
