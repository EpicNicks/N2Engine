#include "engine/ui/Slider.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Window.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::UI
{
    namespace
    {
        bool Same(const Math::Vector2 &a, const Math::Vector2 &b) { return a.x == b.x && a.y == b.y; }

        void SetAnchors(RectTransform &rectTransform, const Math::Vector2 &anchorMin, const Math::Vector2 &anchorMax)
        {
            if (!Same(rectTransform.GetAnchorMin(), anchorMin))
            {
                rectTransform.SetAnchorMin(anchorMin);
            }
            if (!Same(rectTransform.GetAnchorMax(), anchorMax))
            {
                rectTransform.SetAnchorMax(anchorMax);
            }
        }
    }

    Slider::Slider(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("minValue", _minValue);
        RegisterMember("maxValue", _maxValue);
        RegisterMember("value", _value);
        RegisterMember("wholeNumbers", _wholeNumbers);
        RegisterMember("direction", _direction);
        RegisterMember("interactable", _interactable);
        RegisterComponentRef("fillRect", _fillRect).Tooltip("Anchored over its parent's rect up to the value");
        RegisterComponentRef("handleRect", _handleRect).Tooltip("Anchored at the value across its parent's rect");
    }

    bool Slider::IsVertical() const
    {
        return _direction == SliderDirection::BottomToTop || _direction == SliderDirection::TopToBottom;
    }

    bool Slider::IsReversed() const
    {
        return _direction == SliderDirection::RightToLeft || _direction == SliderDirection::TopToBottom;
    }

    float Slider::ClampValue(const float value) const
    {
        const float low = std::min(_minValue, _maxValue);
        const float high = std::max(_minValue, _maxValue);
        float result = std::isnan(value) ? low : std::clamp(value, low, high);
        if (_wholeNumbers)
        {
            result = std::clamp(std::round(result), std::ceil(low), std::floor(high));
        }
        return result;
    }

    float Slider::GetNormalizedValue() const
    {
        const float range = _maxValue - _minValue;
        if (!(std::abs(range) > 0.0f))
        {
            return 0.0f;
        }
        return std::clamp((GetValue() - _minValue) / range, 0.0f, 1.0f);
    }

    void Slider::SetNormalizedValue(const float normalized)
    {
        const float t = std::isfinite(normalized) ? std::clamp(normalized, 0.0f, 1.0f) : 0.0f;
        SetValue(_minValue + (_maxValue - _minValue) * t);
    }

    void Slider::SetValue(const float value)
    {
        Set(value, true);
    }

    void Slider::SetValueWithoutNotify(const float value)
    {
        Set(value, false);
    }

    void Slider::Set(const float value, const bool notify)
    {
        const float previous = ClampValue(_value);
        _value = ClampValue(value);
        UpdateVisuals();
        if (notify && _value != previous)
        {
            // Held for the invoke: a listener may destroy this component. Nothing after it touches the component.
            const std::shared_ptr<Base::EventHandler<float>> onValueChanged = _onValueChanged;
            (*onValueChanged)(_value);
        }
    }

    void Slider::SetMinValue(const float minValue)
    {
        _minValue = minValue;
        Set(_value, true);
    }

    void Slider::SetMaxValue(const float maxValue)
    {
        _maxValue = maxValue;
        Set(_value, true);
    }

    void Slider::SetWholeNumbers(const bool wholeNumbers)
    {
        _wholeNumbers = wholeNumbers;
        Set(_value, true);
    }

    void Slider::SetDirection(const SliderDirection direction)
    {
        _direction = direction;
        UpdateVisuals();
    }

    void Slider::SetFillRect(RectTransform *fill)
    {
        _fillRect = fill;
        UpdateVisuals();
    }

    void Slider::SetHandleRect(RectTransform *handle)
    {
        _handleRect = handle;
        UpdateVisuals();
    }

    void Slider::UpdateVisuals()
    {
        const float n = GetNormalizedValue();
        const bool vertical = IsVertical();
        const bool reversed = IsReversed();
        const float start = reversed ? 1.0f - n : 0.0f;
        const float end = reversed ? 1.0f : n;
        const float at = reversed ? 1.0f - n : n;
        if (_fillRect)
        {
            SetAnchors(*_fillRect, vertical ? Math::Vector2{0.0f, start} : Math::Vector2{start, 0.0f},
                       vertical ? Math::Vector2{1.0f, end} : Math::Vector2{end, 1.0f});
        }
        if (_handleRect)
        {
            SetAnchors(*_handleRect, vertical ? Math::Vector2{0.0f, at} : Math::Vector2{at, 0.0f},
                       vertical ? Math::Vector2{1.0f, at} : Math::Vector2{at, 1.0f});
        }
    }

    void Slider::SetValueFromPointer(const Math::Vector2 &canvasPoint)
    {
        if (!_interactable)
        {
            return;
        }
        const RectTransform *rectTransform = GetGameObject().GetComponent<RectTransform>();
        if (!rectTransform)
        {
            return;
        }
        const Rect &rect = rectTransform->GetRect();
        const float length = IsVertical() ? rect.height : rect.width;
        if (!(length > 0.0f))
        {
            return;
        }
        // Along the bar as it is drawn: a turned or scaled slider is read along its own axis (issue #445)
        const std::optional<Math::Vector2> point = rectTransform->CanvasToLocal(canvasPoint);
        if (!point)
        {
            return;
        }
        float t = IsVertical() ? (point->y - rect.y) / length : (point->x - rect.x) / length;
        if (IsReversed())
        {
            t = 1.0f - t;
        }
        SetNormalizedValue(t);
    }

    Slider::ListenerId Slider::AddOnValueChanged(std::function<void(float)> listener)
    {
        return *_onValueChanged += [listener = std::move(listener)](const float value)
        {
            try
            {
                listener(value);
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::format("Slider OnValueChanged listener threw: {}", e.what()));
            }
            catch (...)
            {
                Logger::Error("Slider OnValueChanged listener threw an unknown exception");
            }
        };
    }

    void Slider::RemoveOnValueChanged(const ListenerId id)
    {
        *_onValueChanged -= id;
    }

    void Slider::ClearOnValueChanged()
    {
        _onValueChanged->Clear();
    }

    std::size_t Slider::GetOnValueChangedListenerCount() const
    {
        return _onValueChanged->GetSubscriberCount();
    }

    void Slider::OnDisable()
    {
        _dragging = false;
    }

    void Slider::OnUpdate()
    {
        if (!IsActive())
        {
            return;
        }
        UpdateVisuals();

        const Input::Mouse *mouse = Input::Mouse::Get();
        if (!mouse)
        {
            return;
        }
        if (!mouse->GetButton(0) || !_interactable)
        {
            _dragging = false;
            return;
        }
        Application &application = Application::GetInstance();
        GameObject &self = GetGameObject();
        if (!_dragging)
        {
            const GameObject *captured = application.GetPointerDispatcher().GetCaptured();
            _dragging = captured && UISystem::IsSelfOrBelow(captured, self);
        }
        if (!_dragging)
        {
            return;
        }
        const Canvas *canvas = UISystem::RootCanvasOf(self);
        if (!canvas || canvas->IsWorldSpace())
        {
            return;
        }
        SetValueFromPointer(UISystem::WindowToCanvas(mouse->GetPosition(), application.GetWindow().GetRenderDimensions()));
    }
}
