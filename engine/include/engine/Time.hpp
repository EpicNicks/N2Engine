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
        static double scaledDeltaTime;
        static double unscaledDeltaTime;

        static double time;
        static double unscaledTime;

        static float timeScale;

        static double fixedDeltaTime;
        static double fixedUnscaledDeltaTime;

        static TimePoint lastFrameTime;

        static void Init();
        static void Update();

    public:
        static float GetDeltaTime();
        static float GetFixedDeltaTime();
        static float GetUnscaledDeltaTime();
        static float GetTime();
        static float GetUnscaledTime();
        static float GetFixedUnscaledDeltaTime();

        static void SetTimeScale(float scale);
        static float GetTimeScale();
    };
}