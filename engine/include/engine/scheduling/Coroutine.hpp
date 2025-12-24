#pragma once

#include <generator>
#include <optional>

#include "engine/scheduling/CoroutineWait.hpp"

namespace N2Engine::Scheduling
{
    class Coroutine
    {
    private:
        bool _isComplete{false};
        std::generator<ICoroutineWait> _gen;
        std::optional<ICoroutineWait> _currentYield;

    public:
        explicit Coroutine(std::generator<ICoroutineWait> gen) : _gen{std::move(gen)} {};

        Coroutine(const Coroutine &) = delete;
        Coroutine &operator=(const Coroutine &) = delete;
        Coroutine(Coroutine &&) = default;
        Coroutine &operator=(Coroutine &&) = default;

        bool IsComplete() const;

        bool MoveNext();
    };
}
