#pragma once

namespace N2Engine::Scripting
{
    class LuaRuntime;
    
    namespace Bindings
    {
        // Core systems
        void BindMath(LuaRuntime& runtime);
        void BindCore(LuaRuntime& runtime);
        void BindUtility(LuaRuntime& runtime);

        // Physics
        void BindPhysics(LuaRuntime& runtime);

        // Audio
        void BindAudio(LuaRuntime& runtime);

        // Input & Events
        void BindInput(LuaRuntime& runtime);
        void BindEvents(LuaRuntime& runtime);

        // Engine services
        void BindTime(LuaRuntime& runtime);
        void BindDebug(LuaRuntime& runtime);
        void BindApplication(LuaRuntime& runtime);
        void BindWindow(LuaRuntime& runtime);
        void BindCamera(LuaRuntime& runtime);
    }
}