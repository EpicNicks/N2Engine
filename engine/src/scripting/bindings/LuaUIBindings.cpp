#include "engine/scripting/bindings/LuaBindings.hpp"

#include <string>
#include <tuple>

#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

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

        // ===== UIText =====
        using UITextRef = ComponentRef<UI::UIText>;
        BindComponentType<UI::UIText>(
            lua, "UIText",
            "SetText", Forward<UITextRef, &UI::UIText::SetText>(),
            "GetText", Forward<UITextRef, &UI::UIText::GetText>(),
            "GetColor", Forward<UITextRef, &UI::UIGraphic::GetColor>(),
            "SetColor", Forward<UITextRef, &UI::UIGraphic::SetColor>(),
            "GetRaycastTarget", Forward<UITextRef, &UI::UIGraphic::GetRaycastTarget>(),
            "SetRaycastTarget", Forward<UITextRef, &UI::UIGraphic::SetRaycastTarget>(),
            "SetFontSize", Forward<UITextRef, &UI::UIText::SetFontSize>(),
            "GetFontSize", Forward<UITextRef, &UI::UIText::GetFontSize>(),
            "SetWrap", Forward<UITextRef, &UI::UIText::SetWrap>(),
            "GetWrap", Forward<UITextRef, &UI::UIText::GetWrap>(),
            "SetLineSpacing", Forward<UITextRef, &UI::UIText::SetLineSpacing>(),
            "GetLineSpacing", Forward<UITextRef, &UI::UIText::GetLineSpacing>(),
            "SetLetterSpacing", Forward<UITextRef, &UI::UIText::SetLetterSpacing>(),
            "GetLetterSpacing", Forward<UITextRef, &UI::UIText::GetLetterSpacing>(),
            // Both names are checked before either is applied, so a bad one changes nothing
            "SetAlignment", [](const UITextRef &c, const std::string &horizontal, const std::string &vertical)
            {
                const Text::HorizontalAlign h = ParseHorizontalAlign(horizontal);
                const Text::VerticalAlign v = ParseVerticalAlign(vertical);
                const auto text = c.Pin();
                text->SetHorizontalAlign(h);
                text->SetVerticalAlign(v);
            },
            "GetAlignment", [](const UITextRef &c)
            {
                const auto text = c.Pin();
                return std::make_tuple(AlignName(text->GetHorizontalAlign()), AlignName(text->GetVerticalAlign()));
            },
            // A font file, e.g. "res://fonts/Title.ttf"; nil goes back to the default font. A path that
            // doesn't load raises an error and keeps the current font.
            "SetFont", [](const UITextRef &c, sol::optional<std::string> path)
            {
                const auto text = c.Pin();
                if (!path)
                {
                    text->SetFont(nullptr);
                    return;
                }
                text->SetFont(LoadFontOrThrow(*path, "UIText:SetFont"));
            },
            // The laid-out block in canvas space, in the rect of the last layout: minX, minY, maxX, maxY
            // (all 0 for empty text)
            "GetBounds", [](const UITextRef &c)
            {
                const auto text = c.Pin();
                const Text::Rect bounds = text->GetBoundsIn(text->GetCurrentRect());
                return std::make_tuple(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);
            }
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
            ),
            // CreateElement with a UIText showing the text
            "CreateText", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateText()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateText(name)); },
                [](const std::string &name, const std::string &text)
                {
                    return GameObjectRef::Owning(UI::UISystem::CreateText(name, text));
                }
            )
        );
    }
}
