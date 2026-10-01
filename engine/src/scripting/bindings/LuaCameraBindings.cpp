#include "engine/Application.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Camera.hpp"
#include "engine/Window.hpp"

#include <math/Ray.hpp>

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
        
        // ===== Ray =====
        lua.new_usertype<Math::Ray>("Ray",
            sol::call_constructor,
            sol::constructors<
                Math::Ray(),
                Math::Ray(const Math::Vector3&, const Math::Vector3&)
            >(),

            "origin", &Math::Ray::origin,
            "direction", &Math::Ray::direction,
            "GetPoint", &Math::Ray::GetPoint
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
            "GetFOV", &Camera::GetFOV,

            // Camera:ScreenPointToRay(x, y[, width, height]): the ray through a point in window coordinates
            // (top-left origin, y down, as Input.GetMousePosition). The viewport defaults to the window's size.
            "ScreenPointToRay", [](const Camera& cam, const float x, const float y,
                                   const sol::optional<int> width, const sol::optional<int> height)
            {
                Vector2i viewport = Application::GetInstance().GetWindow().GetWindowDimensions();
                if (width && height)
                {
                    viewport = Vector2i{*width, *height};
                }
                return cam.ScreenPointToRay(Math::Vector2(x, y), viewport);
            }
        );
        
        // ===== Camera Global Access =====
        // A plain pointer, unlike scenes and input maps: the main camera is created once at startup and lives
        // as long as the Application
        lua["Camera"] = lua.create_table_with(
            "Main", []() -> Camera* {
                return Application::GetInstance().GetMainCamera();
            }
        );
    }
}