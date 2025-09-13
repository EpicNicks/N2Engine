#include "engine/scheduling/CoroutineWait.hpp"
#include "engine/Time.hpp"

using namespace N2Engine::Scheduling;

bool WaitForNextFrame::Wait()
{
    return false;
}

bool WaitForFrames::Wait()
{
    _elapsedFrames++;
    return _elapsedFrames < _waitFrames;
}

bool WaitForSeconds::Wait()
{
    _elapsedSeconds += Time::GetDeltaTime();
    return _elapsedSeconds < _waitSeconds;
}

bool WaitForever::Wait()
{
    return true;
}