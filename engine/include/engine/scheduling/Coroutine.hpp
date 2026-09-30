#pragma once

#include <generator>
#include <optional>
#include <ranges>

#include "engine/scheduling/CoroutineWait.hpp"

namespace N2Engine::Scheduling
{
    /// One running coroutine. The scheduler calls MoveNext once per frame: the first call runs the
    /// body to its first yield; later calls check the yielded wait and resume the body when it's over.
    class Coroutine
    {
    private:
        using Iterator = std::ranges::iterator_t<std::generator<ICoroutineWait>>;

        bool _isComplete{false};
        bool _stopRequested{false};
        std::generator<ICoroutineWait> _gen;
        // The generator's iterator, created by the first resume. begin() may only be called once,
        // so later resumes advance this iterator instead of calling begin() again.
        std::optional<Iterator> _it;
        std::optional<ICoroutineWait> _currentYield;

    public:
        explicit Coroutine(std::generator<ICoroutineWait> gen) : _gen{std::move(gen)} {}

        Coroutine(const Coroutine &) = delete;
        Coroutine &operator=(const Coroutine &) = delete;
        Coroutine(Coroutine &&) = default;
        Coroutine &operator=(Coroutine &&) = default;

        [[nodiscard]] bool IsComplete() const;

        /// Advances by one frame. @returns false once the coroutine has finished
        bool MoveNext();

        /// Stopped coroutines are removed by the scheduler after its current update
        void RequestStop() { _stopRequested = true; }
        [[nodiscard]] bool IsStopRequested() const { return _stopRequested; }
    };
}
