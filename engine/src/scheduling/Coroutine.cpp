#include "engine/scheduling/Coroutine.hpp"
#include "engine/scheduling/CoroutineWait.hpp"

using namespace N2Engine::Scheduling;

bool Coroutine::IsComplete() const
{
    return _isComplete;
}

bool Coroutine::MoveNext()
{
    if (_isComplete)
    {
        return false;
    }

    if (_currentYield.has_value())
    {
        if (_currentYield->Wait())
            return true;
        _currentYield.reset();
    }

    auto it = _gen.begin();
    if (it == _gen.end())
    {
        _isComplete = true;
        return false;
    }

    _currentYield = *it;
    return !_currentYield->Wait();
}