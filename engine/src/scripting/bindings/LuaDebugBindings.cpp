#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindDebug(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();

        lua["Debug"] = lua.create_table_with(
            "Log", [](const std::string& msg) {
                Logger::Info(msg);
            },
            
            "Warn", [](const std::string& msg) {
                Logger::Warn(msg);
            },
            
            "Error", [](const std::string& msg) {
                Logger::Error(msg);
            }

            // "DrawLine", [](const Math::Vector3& start, const Math::Vector3& end,
            //               const Common::Color& color, float duration) {
            //     Debug::DrawLine(start, end, color, duration);
            // },
            //
            // "DrawRay", [](const Math::Vector3& start, const Math::Vector3& direction,
            //              const Common::Color& color, float duration) {
            //     Debug::DrawRay(start, direction, color, duration);
            // },
            //
            // "DrawBox", [](const Math::Vector3& center, const Math::Vector3& size,
            //              const Math::Quaternion& rotation, const Common::Color& color, float duration) {
            //     Debug::DrawBox(center, size, rotation, color, duration);
            // },
            //
            // "DrawSphere", [](const Math::Vector3& center, float radius,
            //                 const Common::Color& color, float duration) {
            //     Debug::DrawSphere(center, radius, color, duration);
            // }
        );
    }
}