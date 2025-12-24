#include "engine/Application.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Camera.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindCamera(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== OrthographicResizeMode Enum =====
        lua.new_enum("OrthographicResizeMode",
            "MaintainVertical", Camera::OrthographicResizeMode::MaintainVertical,
            "MaintainHorizontal", Camera::OrthographicResizeMode::MaintainHorizontal,
            "MaintainLarger", Camera::OrthographicResizeMode::MaintainLarger
        );
        
        // ===== BoundingBox =====
        lua.new_usertype<BoundingBox>("BoundingBox",
            sol::call_constructor,
            sol::constructors<
                BoundingBox(),
                BoundingBox(const Math::Vector3&, const Math::Vector3&)
            >(),
            
            "min", &BoundingBox::min,
            "max", &BoundingBox::max,
            
            "GetCenter", &BoundingBox::GetCenter,
            "GetExtents", &BoundingBox::GetExtents,
            "GetCorner", &BoundingBox::GetCorner
        );
        
        // ===== Frustum =====
        lua.new_usertype<Frustum>("Frustum",
            sol::no_constructor,
            
            "IsVisible", &Frustum::IsVisible
        );
        
        // ===== Camera =====
        lua.new_usertype<Camera>("Camera",
            sol::no_constructor,
            
            // Position and orientation
            "SetPosition", &Camera::SetPosition,
            "SetRotation", &Camera::SetRotation,
            "LookAt", sol::overload(
                static_cast<void(Camera::*)(const Math::Vector3&, const Math::Vector3&)>(&Camera::LookAt),
                [](Camera& cam, const Math::Vector3& target) {
                    cam.LookAt(target);
                }
            ),
            
            "GetPosition", &Camera::GetPosition,
            "GetRotation", &Camera::GetRotation,
            
            // Projection
            "SetPerspective", &Camera::SetPerspective,
            "SetOrthographic", &Camera::SetOrthographic,
            "SetOrthographicResizeMode", &Camera::SetOrthographicResizeMode,
            
            "UpdateAspectRatio", &Camera::UpdateAspectRatio,
            "GetAspectRatio", &Camera::GetAspectRatio,
            
            // Matrices
            "GetViewMatrix", &Camera::GetViewMatrix,
            "GetProjectionMatrix", &Camera::GetProjectionMatrix,
            "GetViewProjectionMatrix", &Camera::GetViewProjectionMatrix,
            
            // Frustum
            "GetViewFrustum", &Camera::GetViewFrustum,
            
            // Properties
            "GetNearPlane", &Camera::GetNearPlane,
            "GetFarPlane", &Camera::GetFarPlane,
            "GetFOV", &Camera::GetFOV
        );
        
        // ===== Camera Global Access =====
        lua["Camera"] = lua.create_table_with(
            "Main", []() -> Camera* {
                return Application::GetInstance().GetMainCamera();
            }
        );
    }
}