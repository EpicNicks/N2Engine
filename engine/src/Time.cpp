#include "engine/Time.hpp"
#include <chrono>

using namespace N2Engine;

Time::TimePoint Time::lastFrameTime = std::chrono::high_resolution_clock::now();
float Time::unscaledTime = 0.0f;
float Time::time = 0.0f;
float Time::timeScale = 1.0f;
float Time::unscaledDeltaTime = 0.0f;

float Time::GetDeltaTime()
{
    return unscaledDeltaTime * timeScale;
}
float Time::GetUnscaledDeltaTime()
{
    return unscaledDeltaTime;
}
float Time::GetTime()
{
    return time;
}
float Time::GetUnscaledTime()
{
    return unscaledTime;
}
void Time::SetTimeScale(float scale)
{
    timeScale = scale;
}
float Time::GetTimeScale()
{
    return timeScale;
}

void Time::Init()
{
    unscaledDeltaTime = 0.0f;
    time = 0.0f;
    unscaledTime = 0.0f;
    timeScale = 1.0f;
    lastFrameTime = std::chrono::high_resolution_clock::now();
}

void Time::Update()
{
    auto currentTime = std::chrono::high_resolution_clock::now();
    float frameTime = std::chrono::duration<float>(currentTime - lastFrameTime).count();
    lastFrameTime = currentTime;

    unscaledDeltaTime = frameTime;
    unscaledTime += frameTime;
    time += frameTime * timeScale;
}
