#include "engine/Time.hpp"
#include <chrono>

using namespace N2Engine;

Time::TimePoint Time::lastFrameTime = std::chrono::high_resolution_clock::now();
double Time::unscaledTime = 0.0;
double Time::time = 0.0;
float Time::timeScale = 1.0f;
double Time::unscaledDeltaTime = 0.0;
double Time::scaledDeltaTime = 0.0;
double Time::fixedUnscaledDeltaTime = 0.02; // default 50Hz physics
double Time::fixedDeltaTime = 0.02;

float Time::GetDeltaTime()
{
    return static_cast<float>(scaledDeltaTime);
}

float Time::GetUnscaledDeltaTime()
{
    return static_cast<float>(unscaledDeltaTime);
}

float Time::GetFixedDeltaTime()
{
    return static_cast<float>(fixedDeltaTime);
}

float Time::GetFixedUnscaledDeltaTime()
{
    return static_cast<float>(fixedUnscaledDeltaTime);
}

float Time::GetTime()
{
    return static_cast<float>(time);
}

float Time::GetUnscaledTime()
{
    return static_cast<float>(unscaledTime);
}

void Time::SetTimeScale(float scale)
{
    timeScale = scale;
    fixedDeltaTime = fixedUnscaledDeltaTime * scale;
}

float Time::GetTimeScale()
{
    return timeScale;
}

void Time::Init()
{
    unscaledDeltaTime = 0.0;
    scaledDeltaTime = 0.0;
    time = 0.0;
    unscaledTime = 0.0;
    timeScale = 1.0f;
    fixedUnscaledDeltaTime = 0.02;
    fixedDeltaTime = 0.02;
    lastFrameTime = std::chrono::high_resolution_clock::now();
}

void Time::Update()
{
    const auto currentTime = std::chrono::high_resolution_clock::now();
    const double frameTime = std::chrono::duration<double>(currentTime - lastFrameTime).count();
    lastFrameTime = currentTime;

    unscaledDeltaTime = frameTime;
    scaledDeltaTime = frameTime * timeScale;

    unscaledTime += frameTime;
    time += scaledDeltaTime;
}
