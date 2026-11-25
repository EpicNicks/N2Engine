#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <math/Vector2.hpp>

#include "engine/base/EventHandler.hpp"
#include "engine/common/Color.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    class RectTransform;
    class UIGraphic;

    /// Which way a Slider's value grows (Unity's Slider.Direction)
    enum class SliderDirection : std::uint8_t
    {
        LeftToRight,
        RightToLeft,
        BottomToTop,
        TopToBottom
    };

    N2_SERIALIZE_ENUM(SliderDirection, {
                      { SliderDirection::LeftToRight, "LeftToRight" },
                      { SliderDirection::RightToLeft, "RightToLeft" },
                      { SliderDirection::BottomToTop, "BottomToTop" },
                      { SliderDirection::TopToBottom, "TopToBottom" }
                      })

    /**
     * A value picked by dragging along a bar (Unity's Slider), issue #445. Its value runs from minValue to maxValue
     * (whole numbers only with wholeNumbers on) along its object's rect, in `direction`.
     *
     * Visuals: an optional fill (fillRect, a RectTransform) is anchored over the part of its parent's rect up to the value,
     * and an optional handle (handleRect) is anchored at the value across its parent's rect, keeping its own size along
     * the bar. Their parents are the "areas" they move in; UISystem::CreateSlider makes the slider's own object that.
     * The visuals follow the value on every change and in OnUpdate (so a value loaded with a scene shows).
     *
     * Input: while the left button is held after a press on the slider or anything below it (its handle, its fill), the
     * value follows the pointer across the slider's rect (OnUpdate reads the application's mouse and PointerDispatcher;
     * screen-space overlay canvases only). SetValueFromPointer does the same for a canvas point, for tests and scripts.
     * A slider that is not interactable, or a disabled component, ignores the pointer.
     *
     * Selection (issue #445, UINavigator): while its object is selected, the arrows along its bar change the value by a
     * tenth of the range (by 1 with wholeNumbers), and its handle (or without one, the first graphic on its object) is
     * tinted selectedColor.
     *
     * OnValueChanged(value) runs whenever the value changes (SetValue, the pointer, a range change that clamps it);
     * SetValueWithoutNotify changes it silently. A listener that throws is logged. Listeners are not saved.
     */
    class Slider final : public SerializableComponent
    {
    public:
        using ListenerId = std::size_t;

        explicit Slider(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Slider"; }

        [[nodiscard]] float GetMinValue() const { return _minValue; }
        /// Clamps the value into the new range (notifying if it changes)
        void SetMinValue(float minValue);
        [[nodiscard]] float GetMaxValue() const { return _maxValue; }
        void SetMaxValue(float maxValue);
        [[nodiscard]] bool GetWholeNumbers() const { return _wholeNumbers; }
        /// Rounds the value (notifying if it changes)
        void SetWholeNumbers(bool wholeNumbers);
        [[nodiscard]] SliderDirection GetDirection() const { return _direction; }
        void SetDirection(SliderDirection direction);
        [[nodiscard]] bool IsInteractable() const { return _interactable; }
        /// The tint of the handle while selected
        [[nodiscard]] const Common::Color &GetSelectedColor() const { return _selectedColor; }
        void SetSelectedColor(const Common::Color &color);
        void SetInteractable(bool interactable) { _interactable = interactable; }
        [[nodiscard]] RectTransform *GetFillRect() const { return _fillRect; }
        void SetFillRect(RectTransform *fill);
        [[nodiscard]] RectTransform *GetHandleRect() const { return _handleRect; }
        void SetHandleRect(RectTransform *handle);

        /// The value, always in range (and whole with wholeNumbers)
        [[nodiscard]] float GetValue() const { return ClampValue(_value); }
        /// Sets the value, clamped and rounded; OnValueChanged runs if it changed
        void SetValue(float value);
        /// Sets it without calling the listeners
        void SetValueWithoutNotify(float value);
        /// 0 at minValue, 1 at maxValue (0 when the range is empty)
        [[nodiscard]] float GetNormalizedValue() const;
        void SetNormalizedValue(float normalized);
        /// The value `value` would be stored as: clamped to the range, rounded with wholeNumbers. NaN counts as minValue.
        [[nodiscard]] float ClampValue(float value) const;

        /// Sets the value for a pointer at `canvasPoint` over the slider's rect (its last resolved one), as a drag does.
        /// Nothing when not interactable or the rect has no length.
        void SetValueFromPointer(const Math::Vector2 &canvasPoint);

        /// Places the fill and handle for the current value (called on every change and in OnUpdate)
        void UpdateVisuals();

        ListenerId AddOnValueChanged(std::function<void(float)> listener);
        void RemoveOnValueChanged(ListenerId id);
        void ClearOnValueChanged();
        [[nodiscard]] std::size_t GetOnValueChangedListenerCount() const;

        void OnUpdate() override;
        void OnDisable() override;
        void OnDestroy() override;
        void OnSelect() override;
        void OnDeselect() override;

        static constexpr bool IsSingleton = true;

    private:
        float _minValue = 0.0f;
        float _maxValue = 1.0f;
        float _value = 0.0f;
        bool _wholeNumbers = false;
        SliderDirection _direction = SliderDirection::LeftToRight;
        bool _interactable = true;
        Common::Color _selectedColor{0.7f, 0.84f, 1.0f, 1.0f};
        bool _selected = false;
        // The graphic tinted while selected, held by its lifetime token
        UIGraphic *_tinted = nullptr;
        std::weak_ptr<const bool> _tintedLifetime;

        /// Tints the selection graphic when selected, and gives a graphic tinted before a white tint back
        void ApplySelectionTint();
        RectTransform *_fillRect = nullptr;
        RectTransform *_handleRect = nullptr;

        bool _dragging = false; // the press was on this slider and the button is still held

        std::shared_ptr<Base::EventHandler<float>> _onValueChanged = std::make_shared<Base::EventHandler<float>>();

        [[nodiscard]] bool IsVertical() const;
        [[nodiscard]] bool IsReversed() const;
        /// Stores `value` (clamped); calls the listeners if `notify` and it changed. Nothing touches the component after them.
        void Set(float value, bool notify);
    };
}
