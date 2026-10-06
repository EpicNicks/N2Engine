#include "engine/ui/Button.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include <math/Vector4.hpp>

#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"
#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    // Unity's ColorBlock.defaultColorBlock (Color32 245, 200, and 200 at alpha 128)
    const Common::Color Button::DefaultNormalColor{1.0f, 1.0f, 1.0f, 1.0f};
    const Common::Color Button::DefaultHighlightedColor{245.0f / 255.0f, 245.0f / 255.0f, 245.0f / 255.0f, 1.0f};
    const Common::Color Button::DefaultPressedColor{200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f};
    const Common::Color Button::DefaultDisabledColor{200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f,
                                                     128.0f / 255.0f};

    // ===== GraphicHandle =====

    UIGraphic *Button::GraphicHandle::Get() const
    {
        if (!graphic || lifetime.expired() || graphic->IsDestroyed())
        {
            return nullptr;
        }
        return graphic;
    }

    void Button::GraphicHandle::Set(UIGraphic *target)
    {
        graphic = target;
        lifetime = target ? target->GetLifetimeToken() : std::weak_ptr<const bool>{};
    }

    void Button::GraphicHandle::Reset()
    {
        graphic = nullptr;
        lifetime.reset();
    }

    // ===== Button =====

    Button::Button(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("interactable", _interactable);
        RegisterMember("normalColor", _normalColor);
        RegisterMember("highlightedColor", _highlightedColor);
        RegisterMember("pressedColor", _pressedColor);
        RegisterMember("disabledColor", _disabledColor);
        RegisterMember("colorMultiplier", _colorMultiplier);
        RegisterMember("fadeDuration", _fadeDuration);
    }

    void Button::SetInteractable(const bool interactable)
    {
        if (_interactable == interactable)
        {
            return;
        }
        _interactable = interactable;
        Transition(false);
    }

    void Button::SetNormalColor(const Common::Color &color)
    {
        _normalColor = color;
        Transition(true);
    }

    void Button::SetHighlightedColor(const Common::Color &color)
    {
        _highlightedColor = color;
        Transition(true);
    }

    void Button::SetPressedColor(const Common::Color &color)
    {
        _pressedColor = color;
        Transition(true);
    }

    void Button::SetDisabledColor(const Common::Color &color)
    {
        _disabledColor = color;
        Transition(true);
    }

    void Button::SetColorMultiplier(const float multiplier)
    {
        _colorMultiplier = multiplier;
        Transition(true);
    }

    void Button::SetFadeDuration(const float seconds)
    {
        _fadeDuration = seconds;
    }

    UIGraphic *Button::GetTargetGraphic() const
    {
        if (_hasExplicitTarget)
        {
            return _explicitTarget.Get();
        }
        for (UIGraphic *graphic : GetGameObject().GetComponents<UIGraphic>())
        {
            if (graphic && !graphic->IsDestroyed())
            {
                return graphic;
            }
        }
        return nullptr;
    }

    void Button::SetTargetGraphic(UIGraphic *graphic)
    {
        _hasExplicitTarget = graphic != nullptr;
        _explicitTarget.Set(graphic);
        ApplyTint();
    }

    Button::State Button::GetState() const
    {
        if (!_interactable)
        {
            return State::Disabled;
        }
        if (_pointerDown && _pointerInside)
        {
            return State::Pressed;
        }
        if (_pointerInside)
        {
            return State::Highlighted;
        }
        return State::Normal;
    }

    Common::Color Button::TintFor(const State state) const
    {
        const Common::Color *color = &_normalColor;
        switch (state)
        {
        case State::Normal:
            color = &_normalColor;
            break;
        case State::Highlighted:
            color = &_highlightedColor;
            break;
        case State::Pressed:
            color = &_pressedColor;
            break;
        case State::Disabled:
            color = &_disabledColor;
            break;
        }
        return Common::Color{color->r * _colorMultiplier, color->g * _colorMultiplier, color->b * _colorMultiplier,
                             color->a * _colorMultiplier};
    }

    void Button::Transition(const bool instant)
    {
        const Common::Color target = TintFor(GetState());
        if (instant || !(_fadeDuration > 0.0f) || target == _currentTint)
        {
            _fading = false;
            _currentTint = target;
        }
        else if (!_fading || !(target == _fadeTo))
        {
            // From wherever the tint is now, so a state change mid-fade doesn't jump
            _fadeFrom = _currentTint;
            _fadeTo = target;
            _fadeElapsed = 0.0f;
            _fading = true;
        }
        ApplyTint();
    }

    void Button::UpdateFade(const float unscaledDeltaTime)
    {
        if (_fading)
        {
            if (std::isfinite(unscaledDeltaTime) && unscaledDeltaTime > 0.0f)
            {
                _fadeElapsed += unscaledDeltaTime;
            }
            const float t = _fadeDuration > 0.0f ? std::clamp(_fadeElapsed / _fadeDuration, 0.0f, 1.0f) : 1.0f;
            if (t >= 1.0f)
            {
                _currentTint = _fadeTo;
                _fading = false;
            }
            else
            {
                _currentTint = Common::Color(Math::Vector4::Lerp(_fadeFrom, _fadeTo, t));
            }
        }
        ApplyTint();
    }

    void Button::ApplyTint()
    {
        UIGraphic *target = GetTargetGraphic();
        if (UIGraphic *previous = _tinted.Get(); previous && previous != target)
        {
            previous->SetTint(Common::Color::White);
        }
        if (target)
        {
            target->SetTint(_currentTint);
        }
        _tinted.Set(target);
    }

    void Button::ClearPointerState()
    {
        _pointerInside = false;
        _pointerDown = false;
    }

    // ===== OnClick =====

    Button::ListenerId Button::AddOnClick(std::function<void()> listener)
    {
        // Each listener is guarded on its own, so one that throws doesn't stop the ones after it
        return *_onClick += [listener = std::move(listener)]()
        {
            try
            {
                listener();
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::format("Button OnClick listener threw: {}", e.what()));
            }
            catch (...)
            {
                Logger::Error("Button OnClick listener threw an unknown exception");
            }
        };
    }

    void Button::RemoveOnClick(const ListenerId id)
    {
        *_onClick -= id;
    }

    void Button::ClearOnClick()
    {
        _onClick->Clear();
    }

    std::size_t Button::GetOnClickListenerCount() const
    {
        return _onClick->GetSubscriberCount();
    }

    bool Button::Click()
    {
        if (!IsActive() || !_interactable)
        {
            return false;
        }
        // Held for the invoke: a listener may destroy this button (RemoveComponent frees it at once)
        const std::shared_ptr<Base::EventHandler<>> onClick = _onClick;
        (*onClick)();
        return true;
    }

    // ===== Lifecycle =====

    void Button::OnUpdate()
    {
        // Also keeps the tint on the right graphic when the target changes (a graphic added, removed or
        // loaded) without a state change
        UpdateFade(Time::GetUnscaledDeltaTime());
    }

    void Button::OnEnable()
    {
        ClearPointerState();
        Transition(true);
    }

    void Button::OnDisable()
    {
        // The dispatcher sends a deactivated object no Exit or Up: forget the hover and the press
        ClearPointerState();
        Transition(true);
    }

    void Button::OnActiveFlagChanged()
    {
        // Enabling or disabling the component itself (no OnEnable/OnDisable for that): as above
        ClearPointerState();
        Transition(true);
    }

    void Button::OnDestroy()
    {
        // Listeners may hold script functions and handles: let them go now, not when the component is freed,
        // and stop an invoke under way from calling any more of them
        _onClick->Clear();
        if (UIGraphic *tinted = _tinted.Get())
        {
            tinted->SetTint(Common::Color::White);
        }
        _tinted.Reset();
        _explicitTarget.Reset();
    }

    // ===== Pointer callbacks (sent to every component of the object, enabled or not) =====

    void Button::OnMouseEnter()
    {
        if (!IsActive())
        {
            return;
        }
        _pointerInside = true;
        Transition(false);
    }

    void Button::OnMouseExit()
    {
        if (!IsActive())
        {
            return;
        }
        _pointerInside = false;
        Transition(false);
    }

    void Button::OnMouseDown()
    {
        if (!IsActive())
        {
            return;
        }
        _pointerDown = true;
        Transition(false);
    }

    void Button::OnMouseUp()
    {
        if (!IsActive())
        {
            return;
        }
        _pointerDown = false;
        Transition(false);
    }

    void Button::OnMouseUpAsButton()
    {
        // Comes before OnMouseUp. Only a press this button saw counts (not one made while it was disabled).
        if (!IsActive() || !_pointerDown)
        {
            return;
        }
        Click();
    }
}
