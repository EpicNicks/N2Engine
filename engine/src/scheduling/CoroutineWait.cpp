#include "engine/scheduling/CoroutineWait.hpp"

using namespace N2Engine::Scheduling;

bool WaitForNextFrame::Wait()
{
    return false;
}

bool WaitForSeconds::Wait()
{
    float deltaTime = 0;
    _elapsedSeconds += deltaTime;
    return _elapsedSeconds < _waitSeconds;
}

bool WaitForever::Wait()
{
    return true;
}