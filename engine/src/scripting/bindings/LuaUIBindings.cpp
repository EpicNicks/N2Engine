#include "engine/scripting/bindings/LuaBindings.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        std::string StateName(const UI::Button::State state)
        {
            switch (state)
            {
            case UI::Button::State::Highlighted:
                return "Highlighted";
            case UI::Button::State::Pressed:
                return "Pressed";
            case UI::Button::State::Disabled:
                return "Disabled";
            case UI::Button::State::Normal:
                break;
            }
            return "Normal";
        }

        /// "ScreenSpaceOverlay" or "WorldSpace"; anything else is an error naming `where`
        UI::CanvasRenderMode ParseRenderMode(const std::string &name, const char *where)
        {
            if (name == "ScreenSpaceOverlay")
            {
                return UI::CanvasRenderMode::ScreenSpaceOverlay;
            }
            if (name == "WorldSpace")
            {
                return UI::CanvasRenderMode::WorldSpace;
            }
            throw std::runtime_error(std::string(where) + ": unknown render mode \"" + name +
                                     "\" (expected \"ScreenSpaceOverlay\" or \"WorldSpace\")");
        }

        std::string RenderModeName(const UI::CanvasRenderMode mode)
        {
            return mode == UI::CanvasRenderMode::WorldSpace ? "WorldSpace" : "ScreenSpaceOverlay";
        }

        /// A UI graphic handle from Lua (Image or UIText), or nullptr for nil; any other value is an error
        UI::UIGraphic *GraphicFromLua(const sol::object &object)
        {
            if (!object.valid() || object.get_type() == sol::type::lua_nil)
            {
                return nullptr;
            }
            if (object.is<ComponentRef<UI::Image>>())
            {
                return object.as<ComponentRef<UI::Image>>().Pin().get();
            }
            if (object.is<ComponentRef<UI::UIText>>())
            {
                return object.as<ComponentRef<UI::UIText>>().Pin().get();
            }
            throw std::runtime_error("Button:SetTargetGraphic: expected an Image, a UIText or nil");
        }
    }

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
            "SetSortOrder", Forward<CanvasRef, &UI::Canvas::SetSortOrder>(),
            // "ScreenSpaceOverlay" or "WorldSpace"; an unknown name raises an error and changes nothing.
            // WorldSpace adds a Positionable and a RectTransform if the object has none.
            "SetRenderMode", [](const CanvasRef &c, const std::string &mode)
            {
                const UI::CanvasRenderMode renderMode = ParseRenderMode(mode, "Canvas:SetRenderMode");
                c.Pin()->SetRenderMode(renderMode);
            },
            "GetRenderMode", [](const CanvasRef &c) { return RenderModeName(c.Pin()->GetRenderMode()); },
            "IsWorldSpace", Forward<CanvasRef, &UI::Canvas::IsWorldSpace>(),
            // A world canvas's size in canvas units (its RectTransform's sizeDelta)
            "GetSize", Forward<CanvasRef, &UI::Canvas::GetSize>(),
            "SetSize", Forward<CanvasRef, &UI::Canvas::SetSize>()
        );

        // ===== Image =====
        using ImageRef = ComponentRef<UI::Image>;
        BindComponentType<UI::Image>(
            lua, "Image",
            "GetColor", Forward<ImageRef, &UI::UIGraphic::GetColor>(),
            "SetColor", Forward<ImageRef, &UI::UIGraphic::SetColor>(),
            "GetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::GetRaycastTarget>(),
            "SetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::SetRaycastTarget>(),
            // An image file, e.g. "res://ui/icon.png"; nil clears the sprite. A path that doesn't load raises an
            // error and keeps the current sprite.
            "SetSprite", [](const ImageRef &c, sol::optional<std::string> path)
            {
                const auto image = c.Pin();
                if (!path)
                {
                    image->SetSprite(nullptr);
                    return;
                }
                image->SetSprite(LoadTextureOrThrow(*path, "Image:SetSprite"));
            },
            // The sprite's path (res:// for a project asset), or nil without a sprite
            "GetSprite", [](const ImageRef &c) -> sol::optional<std::string>
            {
                const auto image = c.Pin();
                if (!image->GetSprite())
                {
                    return sol::nullopt;
                }
                return TexturePathForLua(*image->GetSprite());
            }
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
            // Effects (Text::TextEffects), lengths in ems
            "SetOutline", [](const UITextRef &c, const float width, const Common::Color &color)
            {
                c.Pin()->SetOutline(width, color);
            },
            "GetOutline", [](const UITextRef &c)
            {
                const auto text = c.Pin();
                const Text::TextEffects &effects = text->GetEffects();
                return std::make_tuple(effects.outlineWidth, Common::Color(effects.outlineColor));
            },
            "SetShadow", [](const UITextRef &c, const float offsetX, const float offsetY, const Common::Color &color,
                            const sol::optional<float> softness)
            {
                c.Pin()->SetShadow(Math::Vector2(offsetX, offsetY), color, softness.value_or(0.0f));
            },
            "GetShadow", [](const UITextRef &c)
            {
                const auto text = c.Pin();
                const Text::TextEffects &effects = text->GetEffects();
                return std::make_tuple(effects.shadowOffset.x, effects.shadowOffset.y,
                                       Common::Color(effects.shadowColor), effects.shadowSoftness);
            },
            "SetSoftness", Forward<UITextRef, &UI::UIText::SetSoftness>(),
            "GetSoftness", [](const UITextRef &c) { return c.Pin()->GetEffects().softness; },
            // Extra effect passes (Text::TextPass), lengths in ems
            "AddEffectPass", [](const UITextRef &c, const float offsetX, const float offsetY, const Common::Color &color,
                                const sol::optional<float> width, const sol::optional<float> softness,
                                const sol::optional<int> order)
            {
                c.Pin()->AddEffectPass(Text::TextPass{color, Math::Vector2(offsetX, offsetY), width.value_or(0.0f),
                                                      softness.value_or(0.0f), order.value_or(0)});
            },
            "ClearEffectPasses", [](const UITextRef &c) { c.Pin()->ClearEffectPasses(); },
            "GetEffectPassCount", [](const UITextRef &c) { return static_cast<int>(c.Pin()->GetEffects().passes.size()); },
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

        // ===== Button =====
        using ButtonRef = ComponentRef<UI::Button>;
        BindComponentType<UI::Button>(
            lua, "Button",
            "IsInteractable", Forward<ButtonRef, &UI::Button::IsInteractable>(),
            "SetInteractable", Forward<ButtonRef, &UI::Button::SetInteractable>(),
            "GetNormalColor", Forward<ButtonRef, &UI::Button::GetNormalColor>(),
            "SetNormalColor", Forward<ButtonRef, &UI::Button::SetNormalColor>(),
            "GetHighlightedColor", Forward<ButtonRef, &UI::Button::GetHighlightedColor>(),
            "SetHighlightedColor", Forward<ButtonRef, &UI::Button::SetHighlightedColor>(),
            "GetPressedColor", Forward<ButtonRef, &UI::Button::GetPressedColor>(),
            "SetPressedColor", Forward<ButtonRef, &UI::Button::SetPressedColor>(),
            "GetDisabledColor", Forward<ButtonRef, &UI::Button::GetDisabledColor>(),
            "SetDisabledColor", Forward<ButtonRef, &UI::Button::SetDisabledColor>(),
            "GetColorMultiplier", Forward<ButtonRef, &UI::Button::GetColorMultiplier>(),
            "SetColorMultiplier", Forward<ButtonRef, &UI::Button::SetColorMultiplier>(),
            "GetFadeDuration", Forward<ButtonRef, &UI::Button::GetFadeDuration>(),
            "SetFadeDuration", Forward<ButtonRef, &UI::Button::SetFadeDuration>(),
            "GetCurrentTint", Forward<ButtonRef, &UI::Button::GetCurrentTint>(),
            // "Normal", "Highlighted", "Pressed" or "Disabled"
            "GetState", [](const ButtonRef &c) { return StateName(c.Pin()->GetState()); },
            // The graphic tinted now (Image or UIText), or nil
            "GetTargetGraphic", [](const ButtonRef &c, sol::this_state state) -> sol::object
            {
                const auto button = c.Pin();
                if (UI::UIGraphic *graphic = button->GetTargetGraphic())
                {
                    return ComponentToLua(*graphic, state.lua_state());
                }
                return sol::make_object(state.lua_state(), sol::lua_nil);
            },
            // An Image or UIText to tint, or nil for the first graphic on the button's object
            "SetTargetGraphic", [](const ButtonRef &c, const sol::object &graphic)
            {
                UI::UIGraphic *target = GraphicFromLua(graphic); // checked before the button is changed
                c.Pin()->SetTargetGraphic(target);
            },
            // fn() runs on each click; returns an id for RemoveOnClick. An error in it is logged, and the
            // other listeners still run. A listener a component script added stops once that component is
            // destroyed.
            "AddOnClick", [](const ButtonRef &c, sol::protected_function callback) -> std::size_t
            {
                const auto button = c.Pin();
                return button->AddOnClick(MakeScriptCallback<>(std::move(callback), "Button.OnClick"));
            },
            "RemoveOnClick", Forward<ButtonRef, &UI::Button::RemoveOnClick>(),
            "ClearOnClick", Forward<ButtonRef, &UI::Button::ClearOnClick>(),
            "GetOnClickListenerCount", Forward<ButtonRef, &UI::Button::GetOnClickListenerCount>(),
            // Invokes the listeners if interactable and enabled; returns whether it did
            "Click", Forward<ButtonRef, &UI::Button::Click>()
        );

        // ===== UI (global) =====
        lua["UI"] = lua.create_table_with(
            // A new root object on the UI layer with a Canvas; mode "WorldSpace" also gives it a Positionable
            // scaled to 0.01 and a 100 x 100 RectTransform. An unknown mode raises an error.
            "CreateCanvas", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateCanvas()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateCanvas(name)); },
                [](const std::string &name, const std::string &mode)
                {
                    const UI::CanvasRenderMode renderMode = ParseRenderMode(mode, "UI.CreateCanvas");
                    return GameObjectRef::Owning(UI::UISystem::CreateCanvas(name, renderMode));
                }
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
            ),
            // An element with an Image, a Button and a centred "Label" child showing the label
            "CreateButton", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateButton()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateButton(name)); },
                [](const std::string &name, const std::string &label)
                {
                    return GameObjectRef::Owning(UI::UISystem::CreateButton(name, label));
                }
            )
        );
    }
}
