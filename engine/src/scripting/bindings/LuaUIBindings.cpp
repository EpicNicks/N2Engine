#include "engine/scripting/bindings/LuaBindings.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/RenderTexture.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/LayoutGroup.hpp"
#include "engine/ui/RectMask.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/Slider.hpp"
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

        /// A RectTransform handle from Lua, or nullptr for nil; any other value is an error naming `where`
        UI::RectTransform *RectTransformFromLua(const sol::object &object, const char *where)
        {
            if (!object.valid() || object.get_type() == sol::type::lua_nil)
            {
                return nullptr;
            }
            if (object.is<ComponentRef<UI::RectTransform>>())
            {
                return object.as<ComponentRef<UI::RectTransform>>().Pin().get();
            }
            throw std::runtime_error(std::string(where) + ": expected a RectTransform or nil");
        }

        /// A RectTransform as a Lua handle, or nil
        sol::object RectTransformToLua(UI::RectTransform *rectTransform, lua_State *state)
        {
            if (!rectTransform)
            {
                return sol::make_object(state, sol::lua_nil);
            }
            return ComponentToLua(*rectTransform, state);
        }

        /// "Unrestricted", "Elastic" or "Clamped"; anything else is an error
        UI::ScrollMovementType ParseMovementType(const std::string &name)
        {
            if (name == "Unrestricted")
            {
                return UI::ScrollMovementType::Unrestricted;
            }
            if (name == "Elastic")
            {
                return UI::ScrollMovementType::Elastic;
            }
            if (name == "Clamped")
            {
                return UI::ScrollMovementType::Clamped;
            }
            throw std::runtime_error("ScrollView:SetMovementType: unknown movement type \"" + name +
                                     "\" (expected \"Unrestricted\", \"Elastic\" or \"Clamped\")");
        }

        /// "LeftToRight", "RightToLeft", "BottomToTop" or "TopToBottom"; anything else is an error
        UI::SliderDirection ParseSliderDirection(const std::string &name)
        {
            static constexpr const char *names[] = {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom"};
            for (int i = 0; i < 4; ++i)
            {
                if (name == names[i])
                {
                    return static_cast<UI::SliderDirection>(i);
                }
            }
            throw std::runtime_error("Slider:SetDirection: unknown direction \"" + name +
                                     "\" (expected \"LeftToRight\", \"RightToLeft\", \"BottomToTop\" or \"TopToBottom\")");
        }

        const char *SliderDirectionName(const UI::SliderDirection direction)
        {
            static constexpr const char *names[] = {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom"};
            return names[static_cast<int>(direction)];
        }

        /// A ChildAlignment by name ("UpperLeft" .. "LowerRight"); anything else is an error naming `where`
        UI::ChildAlignment ParseChildAlignment(const std::string &name, const char *where)
        {
            static constexpr const char *names[] = {"UpperLeft", "UpperCenter", "UpperRight", "MiddleLeft", "MiddleCenter",
                                                    "MiddleRight", "LowerLeft", "LowerCenter", "LowerRight"};
            for (int i = 0; i < 9; ++i)
            {
                if (name == names[i])
                {
                    return static_cast<UI::ChildAlignment>(i);
                }
            }
            throw std::runtime_error(std::string(where) + ": unknown alignment \"" + name + "\" (expected UpperLeft .. LowerRight)");
        }

        std::string ChildAlignmentName(const UI::ChildAlignment alignment)
        {
            static constexpr const char *names[] = {"UpperLeft", "UpperCenter", "UpperRight", "MiddleLeft", "MiddleCenter",
                                                    "MiddleRight", "LowerLeft", "LowerCenter", "LowerRight"};
            const auto index = static_cast<int>(alignment);
            return index >= 0 && index < 9 ? names[index] : "UpperLeft";
        }

        UI::GridConstraint ParseGridConstraint(const std::string &name)
        {
            if (name == "Flexible")
            {
                return UI::GridConstraint::Flexible;
            }
            if (name == "FixedColumnCount")
            {
                return UI::GridConstraint::FixedColumnCount;
            }
            if (name == "FixedRowCount")
            {
                return UI::GridConstraint::FixedRowCount;
            }
            throw std::runtime_error("GridLayoutGroup:SetConstraint: unknown constraint \"" + name +
                                     "\" (expected \"Flexible\", \"FixedColumnCount\" or \"FixedRowCount\")");
        }

        std::string GridConstraintName(const UI::GridConstraint constraint)
        {
            switch (constraint)
            {
            case UI::GridConstraint::FixedColumnCount:
                return "FixedColumnCount";
            case UI::GridConstraint::FixedRowCount:
                return "FixedRowCount";
            case UI::GridConstraint::Flexible:
                break;
            }
            return "Flexible";
        }

        UI::FitMode ParseFitMode(const std::string &name, const char *where)
        {
            if (name == "Unconstrained")
            {
                return UI::FitMode::Unconstrained;
            }
            if (name == "PreferredSize")
            {
                return UI::FitMode::PreferredSize;
            }
            throw std::runtime_error(std::string(where) + ": unknown fit mode \"" + name +
                                     "\" (expected \"Unconstrained\" or \"PreferredSize\")");
        }

        std::string FitModeName(const UI::FitMode mode)
        {
            return mode == UI::FitMode::PreferredSize ? "PreferredSize" : "Unconstrained";
        }

        /// The methods every layout group has: padding and child alignment
        template <typename Ref>
        auto PaddingGetter()
        {
            return [](const Ref &c)
            {
                const UI::LayoutPadding padding = c.Pin()->GetPadding();
                return std::make_tuple(padding.left, padding.right, padding.top, padding.bottom);
            };
        }

        template <typename Ref>
        auto PaddingSetter()
        {
            return [](const Ref &c, const float left, const float right, const float top, const float bottom)
            { c.Pin()->SetPadding(UI::LayoutPadding{left, right, top, bottom}); };
        }

        template <typename Ref>
        auto AlignmentGetter()
        {
            return [](const Ref &c) { return ChildAlignmentName(c.Pin()->GetChildAlignment()); };
        }

        template <typename Ref>
        auto AlignmentSetter(const char *where)
        {
            return [where](const Ref &c, const std::string &name)
            {
                const UI::ChildAlignment alignment = ParseChildAlignment(name, where);
                c.Pin()->SetChildAlignment(alignment);
            };
        }

        std::string MovementTypeName(const UI::ScrollMovementType type)
        {
            switch (type)
            {
            case UI::ScrollMovementType::Unrestricted:
                return "Unrestricted";
            case UI::ScrollMovementType::Clamped:
                return "Clamped";
            case UI::ScrollMovementType::Elastic:
                break;
            }
            return "Elastic";
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
            // A RenderTexture (issue #445) to show as the image's sprite, nil for none
            "SetRenderTexture", [](const ImageRef &ref, const sol::optional<std::shared_ptr<Rendering::RenderTexture>> &texture)
            {
                ref.Pin()->SetSprite(texture && *texture ? (*texture)->GetTexture() : nullptr);
            },
            // A user shader (a .n2shader path) for this image, nil for the standard one; the shader's parameters are set on the
            // MaterialInstance of GetMaterialInstance (nil without a shader). Runtime only: not saved in the scene
            "SetShader", [](const ImageRef &ref, const sol::optional<std::string> path)
            { ref.Pin()->SetShader(path ? LoadShaderOrThrow(*path, "Image:SetShader") : nullptr); },
            "GetShader", [](const ImageRef &ref) -> sol::optional<std::string>
            {
                const auto &shader = ref.Pin()->GetShader();
                return shader ? sol::optional<std::string>(ShaderPathForLua(*shader)) : sol::nullopt;
            },
            "GetMaterialInstance", [](const ImageRef &ref, const sol::this_state state)
            { return PrivateMaterialInstance(state, ref.Pin()->GetShaderMaterial()); },
            "SetColor", Forward<ImageRef, &UI::UIGraphic::SetColor>(),
            "GetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::GetRaycastTarget>(),
            "SetRaycastTarget", Forward<ImageRef, &UI::UIGraphic::SetRaycastTarget>(),
            // 9-slicing (issue #445): the texture's border widths in pixels; all 0 stretches the texture over the rect
            "GetSliceBorder", [](const ImageRef &c)
            {
                const Math::Vector4 &border = c.Pin()->GetSliceBorder();
                return std::make_tuple(border.x, border.y, border.z, border.w);
            },
            "SetSliceBorder", [](const ImageRef &c, const float left, const float bottom, const float right, const float top)
            { c.Pin()->SetSliceBorder(left, bottom, right, top); },
            "GetSliceScale", Forward<ImageRef, &UI::Image::GetSliceScale>(),
            "SetSliceScale", Forward<ImageRef, &UI::Image::SetSliceScale>(),
            "GetFillCenter", Forward<ImageRef, &UI::Image::GetFillCenter>(),
            "SetFillCenter", Forward<ImageRef, &UI::Image::SetFillCenter>(),
            "IsSliced", Forward<ImageRef, &UI::Image::IsSliced>(),
            "GetSaturation", Forward<ImageRef, &UI::Image::GetSaturation>(),
            "SetSaturation", Forward<ImageRef, &UI::Image::SetSaturation>(),
            "GetFlash", Forward<ImageRef, &UI::Image::GetFlash>(),
            "SetFlash", Forward<ImageRef, &UI::Image::SetFlash>(),
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
            // Moves character `index` (1-based, in characters of the text) by (x, y) canvas units, +y up (TextMeshPro
            // vertex effects: wave, shake). Not saved; kept across SetText
            "SetCharacterOffset", [](const UITextRef &c, const int index, const float x, const float y)
            {
                if (index >= 1)
                {
                    c.Pin()->SetCharacterOffset(static_cast<std::size_t>(index - 1), Math::Vector2{x, y});
                }
            },
            "ClearCharacterOffsets", [](const UITextRef &c) { c.Pin()->ClearCharacterOffsets(); },
            // Colours character `index` (1-based, as SetCharacterOffset): multiplies the text's colour on that
            // character's face. Not saved; kept across SetText
            "SetCharacterColor", [](const UITextRef &c, const int index, const Common::Color &color)
            {
                if (index >= 1)
                {
                    c.Pin()->SetCharacterColor(static_cast<std::size_t>(index - 1), color);
                }
            },
            "ClearCharacterColors", [](const UITextRef &c) { c.Pin()->ClearCharacterColors(); },
            // Scales character `index` (1-based, as SetCharacterOffset) about its glyph's centre. Not saved; kept
            // across SetText
            "SetCharacterScale", [](const UITextRef &c, const int index, const float scale)
            {
                if (index >= 1)
                {
                    c.Pin()->SetCharacterScale(static_cast<std::size_t>(index - 1), scale);
                }
            },
            "ClearCharacterScales", [](const UITextRef &c) { c.Pin()->ClearCharacterScales(); },
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

        // ===== RectMask (issue #445): clips its object's descendants to its rect; no settings of its own =====
        BindComponentType<UI::RectMask>(lua, "RectMask");

        // ===== ScrollView (issue #445) =====
        using ScrollViewRef = ComponentRef<UI::ScrollView>;
        BindComponentType<UI::ScrollView>(
            lua, "ScrollView",
            // The normalized position: x 0 left .. 1 right, y 0 bottom .. 1 top
            "GetNormalizedPosition", [](const ScrollViewRef &c)
            {
                const Math::Vector2 position = c.Pin()->GetNormalizedPosition();
                return std::make_tuple(position.x, position.y);
            },
            "SetNormalizedPosition", [](const ScrollViewRef &c, const float x, const float y)
            {
                c.Pin()->SetNormalizedPosition(Math::Vector2{x, y});
            },
            "GetHorizontalNormalizedPosition", [](const ScrollViewRef &c) { return c.Pin()->GetNormalizedPosition().x; },
            "SetHorizontalNormalizedPosition", Forward<ScrollViewRef, &UI::ScrollView::SetHorizontalNormalizedPosition>(),
            "GetVerticalNormalizedPosition", [](const ScrollViewRef &c) { return c.Pin()->GetNormalizedPosition().y; },
            "SetVerticalNormalizedPosition", Forward<ScrollViewRef, &UI::ScrollView::SetVerticalNormalizedPosition>(),
            "GetHorizontal", Forward<ScrollViewRef, &UI::ScrollView::GetHorizontal>(),
            "SetHorizontal", Forward<ScrollViewRef, &UI::ScrollView::SetHorizontal>(),
            "GetVertical", Forward<ScrollViewRef, &UI::ScrollView::GetVertical>(),
            "SetVertical", Forward<ScrollViewRef, &UI::ScrollView::SetVertical>(),
            // "Unrestricted", "Elastic" or "Clamped"; an unknown name raises an error and changes nothing
            "GetMovementType", [](const ScrollViewRef &c) { return MovementTypeName(c.Pin()->GetMovementType()); },
            "SetMovementType", [](const ScrollViewRef &c, const std::string &name)
            {
                const UI::ScrollMovementType type = ParseMovementType(name);
                c.Pin()->SetMovementType(type);
            },
            "GetElasticity", Forward<ScrollViewRef, &UI::ScrollView::GetElasticity>(),
            "SetElasticity", Forward<ScrollViewRef, &UI::ScrollView::SetElasticity>(),
            "GetInertia", Forward<ScrollViewRef, &UI::ScrollView::GetInertia>(),
            "SetInertia", Forward<ScrollViewRef, &UI::ScrollView::SetInertia>(),
            "GetDecelerationRate", Forward<ScrollViewRef, &UI::ScrollView::GetDecelerationRate>(),
            "SetDecelerationRate", Forward<ScrollViewRef, &UI::ScrollView::SetDecelerationRate>(),
            "GetScrollSensitivity", Forward<ScrollViewRef, &UI::ScrollView::GetScrollSensitivity>(),
            "SetScrollSensitivity", Forward<ScrollViewRef, &UI::ScrollView::SetScrollSensitivity>(),
            "GetDragThreshold", Forward<ScrollViewRef, &UI::ScrollView::GetDragThreshold>(),
            "SetDragThreshold", Forward<ScrollViewRef, &UI::ScrollView::SetDragThreshold>(),
            "GetVelocity", Forward<ScrollViewRef, &UI::ScrollView::GetVelocity>(),
            "SetVelocity", Forward<ScrollViewRef, &UI::ScrollView::SetVelocity>(),
            "StopMovement", Forward<ScrollViewRef, &UI::ScrollView::StopMovement>(),
            // A wheel step in notches (+y up), as the mouse wheel over the view does
            "Scroll", [](const ScrollViewRef &c, const float x, const float y) { c.Pin()->Scroll(Math::Vector2{x, y}); },
            // The content used (a RectTransform), or nil; SetContent(nil) goes back to the first child
            "GetContent", [](const ScrollViewRef &c, const sol::this_state state)
            { return RectTransformToLua(c.Pin()->GetContent(), state.lua_state()); },
            "SetContent", [](const ScrollViewRef &c, const sol::object &content)
            {
                UI::RectTransform *rectTransform = RectTransformFromLua(content, "ScrollView:SetContent");
                c.Pin()->SetContent(rectTransform);
            },
            "GetVerticalScrollbar", [](const ScrollViewRef &c, const sol::this_state state)
            { return RectTransformToLua(c.Pin()->GetVerticalScrollbar(), state.lua_state()); },
            "SetVerticalScrollbar", [](const ScrollViewRef &c, const sol::object &handle)
            {
                UI::RectTransform *rectTransform = RectTransformFromLua(handle, "ScrollView:SetVerticalScrollbar");
                c.Pin()->SetVerticalScrollbar(rectTransform);
            },
            "GetHorizontalScrollbar", [](const ScrollViewRef &c, const sol::this_state state)
            { return RectTransformToLua(c.Pin()->GetHorizontalScrollbar(), state.lua_state()); },
            "SetHorizontalScrollbar", [](const ScrollViewRef &c, const sol::object &handle)
            {
                UI::RectTransform *rectTransform = RectTransformFromLua(handle, "ScrollView:SetHorizontalScrollbar");
                c.Pin()->SetHorizontalScrollbar(rectTransform);
            },
            // fn(x, y) whenever the content moves, with the new normalized position; returns an id for
            // RemoveOnValueChanged. An error in it is logged. A listener a component script added stops with that component.
            "AddOnValueChanged", [](const ScrollViewRef &c, sol::protected_function callback) -> std::size_t
            {
                const auto scrollView = c.Pin();
                auto call = MakeScriptCallback<float, float>(std::move(callback), "ScrollView.OnValueChanged");
                return scrollView->AddOnValueChanged([call = std::move(call)](const Math::Vector2 position)
                                                     { call(position.x, position.y); });
            },
            "RemoveOnValueChanged", Forward<ScrollViewRef, &UI::ScrollView::RemoveOnValueChanged>(),
            "ClearOnValueChanged", Forward<ScrollViewRef, &UI::ScrollView::ClearOnValueChanged>(),
            "GetOnValueChangedListenerCount", Forward<ScrollViewRef, &UI::ScrollView::GetOnValueChangedListenerCount>()
        );

        // ===== Slider (issue #445) =====
        using SliderRef = ComponentRef<UI::Slider>;
        BindComponentType<UI::Slider>(
            lua, "Slider",
            "GetValue", Forward<SliderRef, &UI::Slider::GetValue>(),
            // Clamped to the range (and rounded with wholeNumbers); the listeners run if it changed
            "SetValue", Forward<SliderRef, &UI::Slider::SetValue>(),
            "SetValueWithoutNotify", Forward<SliderRef, &UI::Slider::SetValueWithoutNotify>(),
            "GetNormalizedValue", Forward<SliderRef, &UI::Slider::GetNormalizedValue>(),
            "SetNormalizedValue", Forward<SliderRef, &UI::Slider::SetNormalizedValue>(),
            "GetMinValue", Forward<SliderRef, &UI::Slider::GetMinValue>(),
            "SetMinValue", Forward<SliderRef, &UI::Slider::SetMinValue>(),
            "GetMaxValue", Forward<SliderRef, &UI::Slider::GetMaxValue>(),
            "SetMaxValue", Forward<SliderRef, &UI::Slider::SetMaxValue>(),
            "GetWholeNumbers", Forward<SliderRef, &UI::Slider::GetWholeNumbers>(),
            "SetWholeNumbers", Forward<SliderRef, &UI::Slider::SetWholeNumbers>(),
            "GetInteractable", Forward<SliderRef, &UI::Slider::IsInteractable>(),
            "SetInteractable", Forward<SliderRef, &UI::Slider::SetInteractable>(),
            "GetDirection", [](const SliderRef &c) { return std::string(SliderDirectionName(c.Pin()->GetDirection())); },
            "SetDirection", [](const SliderRef &c, const std::string &name)
            {
                const UI::SliderDirection direction = ParseSliderDirection(name);
                c.Pin()->SetDirection(direction);
            },
            "GetFillRect", [](const SliderRef &c, const sol::this_state state)
            { return RectTransformToLua(c.Pin()->GetFillRect(), state.lua_state()); },
            "SetFillRect", [](const SliderRef &c, const sol::object &fill)
            {
                UI::RectTransform *rectTransform = RectTransformFromLua(fill, "Slider:SetFillRect");
                c.Pin()->SetFillRect(rectTransform);
            },
            "GetHandleRect", [](const SliderRef &c, const sol::this_state state)
            { return RectTransformToLua(c.Pin()->GetHandleRect(), state.lua_state()); },
            "SetHandleRect", [](const SliderRef &c, const sol::object &handle)
            {
                UI::RectTransform *rectTransform = RectTransformFromLua(handle, "Slider:SetHandleRect");
                c.Pin()->SetHandleRect(rectTransform);
            },
            // For tests and scripts: the value a pointer at this canvas point would set
            "SetValueFromPointer", [](const SliderRef &c, const float x, const float y)
            { c.Pin()->SetValueFromPointer(Math::Vector2{x, y}); },
            // fn(value) whenever the value changes; returns an id for RemoveOnValueChanged. An error in it is logged.
            "AddOnValueChanged", [](const SliderRef &c, sol::protected_function callback) -> std::size_t
            {
                const auto slider = c.Pin();
                auto call = MakeScriptCallback<float>(std::move(callback), "Slider.OnValueChanged");
                return slider->AddOnValueChanged([call = std::move(call)](const float value) { call(value); });
            },
            "RemoveOnValueChanged", Forward<SliderRef, &UI::Slider::RemoveOnValueChanged>(),
            "ClearOnValueChanged", Forward<SliderRef, &UI::Slider::ClearOnValueChanged>(),
            "GetOnValueChangedListenerCount", Forward<SliderRef, &UI::Slider::GetOnValueChangedListenerCount>()
        );

        // ===== Layout groups, LayoutElement, ContentSizeFitter (issue #445) =====
        using HorizontalRef = ComponentRef<UI::HorizontalLayoutGroup>;
        BindComponentType<UI::HorizontalLayoutGroup>(
            lua, "HorizontalLayoutGroup",
            "GetPadding", PaddingGetter<HorizontalRef>(),
            "SetPadding", PaddingSetter<HorizontalRef>(),
            "GetChildAlignment", AlignmentGetter<HorizontalRef>(),
            "SetChildAlignment", AlignmentSetter<HorizontalRef>("HorizontalLayoutGroup:SetChildAlignment"),
            "GetSpacing", Forward<HorizontalRef, &UI::HorizontalOrVerticalLayoutGroup::GetSpacing>(),
            "SetSpacing", Forward<HorizontalRef, &UI::HorizontalOrVerticalLayoutGroup::SetSpacing>(),
            "GetControlChildSize", [](const HorizontalRef &c)
            {
                const auto group = c.Pin();
                return std::make_tuple(group->GetControlChildWidth(), group->GetControlChildHeight());
            },
            "SetControlChildSize", [](const HorizontalRef &c, const bool width, const bool height)
            {
                const auto group = c.Pin();
                group->SetControlChildWidth(width);
                group->SetControlChildHeight(height);
            },
            "GetChildForceExpand", [](const HorizontalRef &c)
            {
                const auto group = c.Pin();
                return std::make_tuple(group->GetChildForceExpandWidth(), group->GetChildForceExpandHeight());
            },
            "SetChildForceExpand", [](const HorizontalRef &c, const bool width, const bool height)
            {
                const auto group = c.Pin();
                group->SetChildForceExpandWidth(width);
                group->SetChildForceExpandHeight(height);
            },
            "GetPreferredSize", [](const HorizontalRef &c)
            {
                const auto group = c.Pin();
                const Math::Vector2 size = group->GetPreferredSize(UI::Rect{});
                return std::make_tuple(size.x, size.y);
            }
        );
        using VerticalRef = ComponentRef<UI::VerticalLayoutGroup>;
        BindComponentType<UI::VerticalLayoutGroup>(
            lua, "VerticalLayoutGroup",
            "GetPadding", PaddingGetter<VerticalRef>(),
            "SetPadding", PaddingSetter<VerticalRef>(),
            "GetChildAlignment", AlignmentGetter<VerticalRef>(),
            "SetChildAlignment", AlignmentSetter<VerticalRef>("VerticalLayoutGroup:SetChildAlignment"),
            "GetSpacing", Forward<VerticalRef, &UI::HorizontalOrVerticalLayoutGroup::GetSpacing>(),
            "SetSpacing", Forward<VerticalRef, &UI::HorizontalOrVerticalLayoutGroup::SetSpacing>(),
            "GetControlChildSize", [](const VerticalRef &c)
            {
                const auto group = c.Pin();
                return std::make_tuple(group->GetControlChildWidth(), group->GetControlChildHeight());
            },
            "SetControlChildSize", [](const VerticalRef &c, const bool width, const bool height)
            {
                const auto group = c.Pin();
                group->SetControlChildWidth(width);
                group->SetControlChildHeight(height);
            },
            "GetChildForceExpand", [](const VerticalRef &c)
            {
                const auto group = c.Pin();
                return std::make_tuple(group->GetChildForceExpandWidth(), group->GetChildForceExpandHeight());
            },
            "SetChildForceExpand", [](const VerticalRef &c, const bool width, const bool height)
            {
                const auto group = c.Pin();
                group->SetChildForceExpandWidth(width);
                group->SetChildForceExpandHeight(height);
            },
            "GetPreferredSize", [](const VerticalRef &c)
            {
                const auto group = c.Pin();
                const Math::Vector2 size = group->GetPreferredSize(UI::Rect{});
                return std::make_tuple(size.x, size.y);
            }
        );
        using GridRef = ComponentRef<UI::GridLayoutGroup>;
        BindComponentType<UI::GridLayoutGroup>(
            lua, "GridLayoutGroup",
            "GetPadding", PaddingGetter<GridRef>(),
            "SetPadding", PaddingSetter<GridRef>(),
            "GetChildAlignment", AlignmentGetter<GridRef>(),
            "SetChildAlignment", AlignmentSetter<GridRef>("GridLayoutGroup:SetChildAlignment"),
            "GetCellSize", Forward<GridRef, &UI::GridLayoutGroup::GetCellSize>(),
            "SetCellSize", Forward<GridRef, &UI::GridLayoutGroup::SetCellSize>(),
            "GetSpacing", Forward<GridRef, &UI::GridLayoutGroup::GetSpacing>(),
            "SetSpacing", Forward<GridRef, &UI::GridLayoutGroup::SetSpacing>(),
            // "Flexible", "FixedColumnCount" or "FixedRowCount"; an unknown name raises an error and changes nothing
            "GetConstraint", [](const GridRef &c) { return GridConstraintName(c.Pin()->GetConstraint()); },
            "SetConstraint", [](const GridRef &c, const std::string &name)
            {
                const UI::GridConstraint constraint = ParseGridConstraint(name);
                c.Pin()->SetConstraint(constraint);
            },
            "GetConstraintCount", Forward<GridRef, &UI::GridLayoutGroup::GetConstraintCount>(),
            "SetConstraintCount", Forward<GridRef, &UI::GridLayoutGroup::SetConstraintCount>()
        );
        using LayoutElementRef = ComponentRef<UI::LayoutElement>;
        BindComponentType<UI::LayoutElement>(
            lua, "LayoutElement",
            "GetPreferredWidth", Forward<LayoutElementRef, &UI::LayoutElement::GetPreferredWidth>(),
            "SetPreferredWidth", Forward<LayoutElementRef, &UI::LayoutElement::SetPreferredWidth>(),
            "GetPreferredHeight", Forward<LayoutElementRef, &UI::LayoutElement::GetPreferredHeight>(),
            "SetPreferredHeight", Forward<LayoutElementRef, &UI::LayoutElement::SetPreferredHeight>(),
            "GetIgnoreLayout", Forward<LayoutElementRef, &UI::LayoutElement::GetIgnoreLayout>(),
            "SetIgnoreLayout", Forward<LayoutElementRef, &UI::LayoutElement::SetIgnoreLayout>()
        );
        using FitterRef = ComponentRef<UI::ContentSizeFitter>;
        BindComponentType<UI::ContentSizeFitter>(
            lua, "ContentSizeFitter",
            // "Unconstrained" or "PreferredSize"; an unknown name raises an error and changes nothing
            "GetHorizontalFit", [](const FitterRef &c) { return FitModeName(c.Pin()->GetHorizontalFit()); },
            "SetHorizontalFit", [](const FitterRef &c, const std::string &name)
            {
                const UI::FitMode mode = ParseFitMode(name, "ContentSizeFitter:SetHorizontalFit");
                c.Pin()->SetHorizontalFit(mode);
            },
            "GetVerticalFit", [](const FitterRef &c) { return FitModeName(c.Pin()->GetVerticalFit()); },
            "SetVerticalFit", [](const FitterRef &c, const std::string &name)
            {
                const UI::FitMode mode = ParseFitMode(name, "ContentSizeFitter:SetVerticalFit");
                c.Pin()->SetVerticalFit(mode);
            }
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
            // Issue #445: a 300 x 200 view with a RectMask, a ScrollView, a "Content" child and scrollbars
            "CreateScrollView", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateScrollView()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateScrollView(name)); },
                [](const std::string &name, const bool vertical)
                { return GameObjectRef::Owning(UI::UISystem::CreateScrollView(name, vertical)); },
                [](const std::string &name, const bool vertical, const bool horizontal)
                { return GameObjectRef::Owning(UI::UISystem::CreateScrollView(name, vertical, horizontal)); }
            ),
            // Issue #445: a 160 x 20 bar with a Slider, a "Fill" child and a "Handle" child
            "CreateSlider", sol::overload(
                []() { return GameObjectRef::Owning(UI::UISystem::CreateSlider()); },
                [](const std::string &name) { return GameObjectRef::Owning(UI::UISystem::CreateSlider(name)); }
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
