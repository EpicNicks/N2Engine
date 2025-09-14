#pragma once
#include <chrono>

namespace N2Engine
{
    class Application;

    class Time
    {
        friend class Application;

        using TimePoint = std::chrono::high_resolution_clock::time_point;

    private:
        static float unscaledDeltaTime;
        static float time;
        static float unscaledTime;
        static float timeScale;
        static TimePoint lastFrameTime;

        static void Init();
        static void Update();

    public:
        static float GetDeltaTime();
        static float GetUnscaledDeltaTime();
        static float GetTime();
        static float GetUnscaledTime();
        static void SetTimeScale(float scale);
        static float GetTimeScale();
    };
}