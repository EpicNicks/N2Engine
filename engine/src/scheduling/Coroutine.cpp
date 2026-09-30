#include "engine/scheduling/Coroutine.hpp"
#include "engine/scheduling/CoroutineWait.hpp"

using namespace N2Engine::Scheduling;

bool Coroutine::IsComplete() const
{
    return _isComplete;
}

bool Coroutine::MoveNext()
{
    if (_isComplete || _stopRequested)
    {
        return false;
    }

    // Still waiting on the last yield? Wait() is checked once per frame.
    if (_currentYield.has_value())
    {
        if (_currentYield->Wait())
        {
            return true;
        }
        _currentYield.reset();
    }

    // Resume the body until its next yield (or the end)
    if (!_it.has_value())
    {
        _it.emplace(_gen.begin());
    }
    else
    {
        ++*_it;
    }

    if (*_it == _gen.end())
    {
        _isComplete = true;
        return false;
    }

    // Hold the yielded wait; it's first checked next frame. (This used to check it immediately and
    // report "complete" whenever it said "keep waiting", so every real wait ended the coroutine.)
    _currentYield.emplace(std::move(**_it));
    return true;
}
