#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "engine/base/EventHandler.hpp"
#include "engine/common/Color.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::UI
{
    class UIGraphic;

    /**
     * A clickable UI element (Unity's Selectable + Button): put it on an object with a raycast-target graphic,
     * typically an Image (UISystem::CreateButton makes one with a label). It follows the pointer through the
     * OnMouse* callbacks that Input::PointerDispatcher sends its object, tints its target graphic for the
     * current state, and invokes OnClick when the left button is pressed and released over it.
     *
     * State (GetState), worked out from what the pointer did:
     * - Disabled while not interactable;
     * - Pressed while the button went down on it and the pointer is over it (dragging off shows Normal);
     * - Highlighted while the pointer is over it;
     * - Normal otherwise.
     *
     * Tint: the target graphic is drawn with its own colour multiplied by the state's colour times
     * colorMultiplier (Unity's ColorBlock), through UIGraphic::SetTint, so the graphic's colour itself is
     * never changed and comes back as it was once the state's colour is white. With fadeDuration > 0 the
     * tint moves from the old state's colour to the new one's over that many seconds of unscaled time
     * (OnUpdate, or UpdateFade by hand); 0 changes it at once.
     *
     * Target graphic: the one set with SetTargetGraphic, held by its lifetime token (no tint once it is
     * freed), or, while none is set, the first graphic on the button's own object, looked up each time.
     *
     * OnClick runs on OnMouseUpAsButton (released over the object the button went down on) when the button
     * is interactable and the component is enabled on an active object, and only if it saw the press. A
     * disabled Button ignores the pointer entirely; disabling it (or deactivating its object) forgets the
     * hover and the press, as Unity's does.
     *
     * Listeners are not serialized (Unity's persistent listeners are out of scope): add them from code or
     * scripts each time the scene is built or loaded.
     */
    class Button final : public SerializableComponent
    {
    public:
        enum class State
        {
            Normal,
            Highlighted,
            Pressed,
            Disabled
        };

        /// Identifies a listener for RemoveOnClick
        using ListenerId = std::size_t;

        // Unity's ColorBlock defaults
        static const Common::Color DefaultNormalColor;
        static const Common::Color DefaultHighlightedColor;
        static const Common::Color DefaultPressedColor;
        static const Common::Color DefaultDisabledColor;
        static constexpr float DefaultColorMultiplier = 1.0f;
        static constexpr float DefaultFadeDuration = 0.1f;

        explicit Button(GameObject &gameObject);
        /// A button freed without OnDestroy (its object never joined a scene) still un-tints its graphic
        ~Button() override;

        [[nodiscard]] std::string GetTypeName() const override { return "Button"; }

        // ===== Interactable =====
        [[nodiscard]] bool IsInteractable() const { return _interactable; }
        /// False shows disabledColor and ignores clicks (the hover and press are still followed, so turning
        /// it back on shows the right state)
        void SetInteractable(bool interactable);

        // ===== Colours (multiplied with the target graphic's own colour) =====
        [[nodiscard]] const Common::Color &GetNormalColor() const { return _normalColor; }
        void SetNormalColor(const Common::Color &color);
        [[nodiscard]] const Common::Color &GetHighlightedColor() const { return _highlightedColor; }
        void SetHighlightedColor(const Common::Color &color);
        [[nodiscard]] const Common::Color &GetPressedColor() const { return _pressedColor; }
        void SetPressedColor(const Common::Color &color);
        [[nodiscard]] const Common::Color &GetDisabledColor() const { return _disabledColor; }
        void SetDisabledColor(const Common::Color &color);
        /// Multiplies every state's colour (1 by default; Unity's colorMultiplier)
        [[nodiscard]] float GetColorMultiplier() const { return _colorMultiplier; }
        void SetColorMultiplier(float multiplier);
        /// Seconds (unscaled) a tint change takes; 0 or less is instant
        [[nodiscard]] float GetFadeDuration() const { return _fadeDuration; }
        void SetFadeDuration(float seconds);

        // ===== Target graphic =====
        /// The graphic tinted now: the one set (nullptr once it is destroyed), else the first live graphic on
        /// this object, or nullptr
        [[nodiscard]] UIGraphic *GetTargetGraphic() const;
        /// Tints this graphic (any object's) instead of the first one on this object; nullptr goes back to
        /// that default. The graphic that was tinted before gets a white tint back.
        void SetTargetGraphic(UIGraphic *graphic);

        // ===== State and tint =====
        [[nodiscard]] State GetState() const;
        /// The tint applied to the target graphic now (mid-fade, between the two states' colours)
        [[nodiscard]] const Common::Color &GetCurrentTint() const { return _currentTint; }
        /// Whether a fade is under way
        [[nodiscard]] bool IsFading() const { return _fading; }
        /// Advances a fade by `unscaledDeltaTime` seconds and applies the tint. OnUpdate calls it with
        /// Time::GetUnscaledDeltaTime(); tests call it with a fixed step.
        void UpdateFade(float unscaledDeltaTime);

        // ===== OnClick =====
        /**
         * Adds a listener; returns the id RemoveOnClick takes. Safe from inside a listener (it is first called
         * on the next click). A listener that throws is logged (Logger::Error) and the others still run.
         */
        ListenerId AddOnClick(std::function<void()> listener);
        /// Removes a listener. Safe from inside a listener, itself included: it isn't called again.
        void RemoveOnClick(ListenerId id);
        /// Removes every listener (also safe from inside one: the rest of that click calls none of them)
        void ClearOnClick();
        [[nodiscard]] std::size_t GetOnClickListenerCount() const;
        /**
         * Invokes the listeners if the button is interactable and the component is active (what a click does;
         * Unity's Press). Returns whether it did. The listeners may destroy the button: the invoke goes on,
         * with any listeners left, but nothing after it touches the button.
         */
        bool Click();

        // ===== Lifecycle and pointer callbacks =====
        void OnAttach() override;
        void OnUpdate() override;
        void OnEnable() override;
        void OnDisable() override;
        void OnDestroy() override;
        void OnMouseEnter() override;
        void OnMouseExit() override;
        void OnMouseDown() override;
        void OnMouseUp() override;
        void OnMouseUpAsButton() override;

        static constexpr bool IsSingleton = false;

    protected:
        void OnActiveFlagChanged() override;

    private:
        /// A graphic held without owning it: usable while its component lives
        struct GraphicHandle
        {
            UIGraphic *graphic = nullptr;
            std::weak_ptr<const bool> lifetime;

            [[nodiscard]] UIGraphic *Get() const;
            void Set(UIGraphic *target);
            void Reset();
        };

        // Serialized settings
        Common::Color _normalColor{DefaultNormalColor};
        Common::Color _highlightedColor{DefaultHighlightedColor};
        Common::Color _pressedColor{DefaultPressedColor};
        Common::Color _disabledColor{DefaultDisabledColor};
        float _colorMultiplier = DefaultColorMultiplier;
        float _fadeDuration = DefaultFadeDuration;
        bool _interactable = true;

        // Pointer state, followed while the component is active
        bool _pointerInside = false;
        bool _pointerDown = false;

        // Target graphic
        bool _hasExplicitTarget = false;
        GraphicHandle _explicitTarget;
        GraphicHandle _tinted; // the graphic the tint was last applied to

        // Tint and fade
        Common::Color _currentTint{Common::Color::White};
        Common::Color _fadeFrom{Common::Color::White};
        Common::Color _fadeTo{Common::Color::White};
        float _fadeElapsed = 0.0f;
        bool _fading = false;

        // Shared, so a listener that destroys the button doesn't free the event it is being called from
        std::shared_ptr<Base::EventHandler<>> _onClick = std::make_shared<Base::EventHandler<>>();

        /// The tint a state shows: its colour times the multiplier
        [[nodiscard]] Common::Color TintFor(State state) const;
        /// TintFor(GetState()), or white while the component is disabled or its object inactive (as Unity
        /// clears a disabled Selectable's tint)
        [[nodiscard]] Common::Color TargetTint() const;
        /// Moves the tint towards the current state's (at once if `instant` or fadeDuration <= 0) and applies it
        void Transition(bool instant);
        /// Sets the current tint on the target graphic, and white on a graphic tinted before that isn't the
        /// target any more
        void ApplyTint();
        /// Forgets the hover and press (on disable)
        void ClearPointerState();
    };
}
