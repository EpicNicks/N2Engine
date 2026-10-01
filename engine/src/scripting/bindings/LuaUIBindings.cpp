#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindUI(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // ===== Rect (a value: what RectTransform:GetRect returns) =====
        lua.new_usertype<UI::Rect>(
            "Rect",
            sol::no_constructor,
            "x", &UI::Rect::x,
            "y", &UI::Rect::y,
            "width", &UI::Rect::width,
            "height", &UI::Rect::height,
            "Contains", &UI::Rect::Contains
        );

        // ===== RectTransform =====
        using RectTransformRef = ComponentRef<UI::RectTransform>;
        BindComponentType<UI::RectTransform>(
            lua, "RectTransform",
            "GetAnchorMin", Forward<RectTransformRef, &UI::RectTransform::GetAnchorMin>(),
            "SetAnchorMin", Forward<RectTransformRef, &UI::RectTransform::SetAnchorMin>(),
            "GetAnchorMax", Forward<RectTransformRef, &UI::RectTransform::GetAnchorMax>(),
            "SetAnchorMax", Forward<RectTransformRef, &UI::RectTransform::SetAnchorMax>(),
            "GetPivot", Forward<RectTransformRef, &UI::RectTransform::GetPivot>(),
            "SetPivot", Forward<RectTransformRef, &UI::RectTransform::SetPivot>(),
            "GetAnchoredPosition", Forward<RectTransformRef, &UI::RectTransform::GetAnchoredPosition>(),
            "SetAnchoredPosition", Forward<RectTransformRef, &UI::RectTransform::SetAnchoredPosition>(),
            "GetSizeDelta", Forward<RectTransformRef, &UI::RectTransform::GetSizeDelta>(),
            "SetSizeDelta", Forward<RectTransformRef, &UI::RectTransform::SetSizeDelta>(),
            "GetOffsetMin", Forward<RectTransformRef, &UI::RectTransform::GetOffsetMin>(),
            "SetOffsetMin", Forward<RectTransformRef, &UI::RectTransform::SetOffsetMin>(),
            "GetOffsetMax", Forward<RectTransformRef, &UI::RectTransform::GetOffsetMax>(),
            "SetOffsetMax", Forward<RectTransformRef, &UI::RectTransform::SetOffsetMax>(),
            "StretchToParent", Forward<RectTransformRef, &UI::RectTransform::StretchToParent>(),
            "GetRect", Forward<RectTransformRef, &UI::RectTransform::GetRect>()
        );

        // ===== Canvas =====
        using CanvasRef = ComponentRef<UI::Canvas>;
        BindComponentType<UI::Canvas>(
            lua, "Canvas",
            "GetSortOrder", Forward<CanvasRef, &UI::Canvas::GetSortOrder>(),
            "SetSortOrder", Forward<CanvasRef, &UI::Canvas::SetSortOrder>()
        );

        // ===== Image =====
        using ImageRef = ComponentRef<UI::Image>;
        BindComponentType<UI::Image>(
            lua, "Image",
            "GetColor", Forward<ImageRef, &UI::UIGraphic::GetColor>(),
            "SetColor", Forward<ImageRef, &UI::UIGraphic::SetColor>(),
            "GetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::GetRaycastTarget>(),
            "SetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::SetRaycastTarget>()
        );

        // ===== UI (global) =====
        lua["UI"] = lua.create_table_with(
            // A new root object on the UI layer with a Canvas
            "CreateCanvas", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateCanvas()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateCanvas(name)); }
            ),
            // A new object on the UI layer with a RectTransform
            "CreateElement", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateElement()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateElement(name)); }
            )
        );
    }
}
