#pragma once

#include <array>
#include <memory>
#include <vector>

#include <renderer/common/Renderer.hpp>

#include "engine/ui/Navigation.hpp"

namespace N2Engine
{
    class GameObject;
    class Scene;
}

namespace N2Engine::UI
{
    /// One frame of keyboard and gamepad input as the navigator reads it. The directions and Tab are "held" states (the
    /// navigator repeats them); submit and cancel are the frame they were pressed.
    struct NavigationInput
    {
        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool next = false;     // Tab
        bool previous = false; // Shift+Tab
        bool submit = false;
        bool cancel = false;
        /// The object the left mouse button went down on this frame, if any
        GameObject *pointerPressed = nullptr;
    };

    /**
     * Keyboard and gamepad navigation of the UI (issue #445; Unity's EventSystem selection). One element at a time is
     * selected; while one is, the arrow keys, WASD aside, and a gamepad's d-pad and left stick move the selection (or
     * change a selected Slider along its bar), Tab and Shift+Tab go to the next and previous element, Enter, keypad Enter,
     * Space and the gamepad's South button submit (click a selected Button), and Escape and the East button cancel.
     *
     * Nothing is selected until a script selects something (SetSelected), so a game that never does sees no change: its
     * keys and buttons are not taken by the UI, and clicking selects nothing. Once something is selected, a click on
     * another selectable element selects that one. The input is not consumed: action maps and Input.GetKey see it too.
     *
     * Selectable elements are active objects with a RectTransform and an enabled, interactable Button or Slider, on a
     * screen-space overlay canvas, or on the world-space canvas the selection is on. A Navigation component changes how
     * the selection moves from one (Explicit neighbours) or keeps the arrows off it (None). Any object can be selected by
     * a script; the selection is dropped when the object is destroyed or deactivated.
     *
     * Automatic moves pick, among the elements whose centre lies in the pressed direction from the selected element's edge,
     * the one with the largest dot(direction, offset) / |offset|^2 (Unity's score), using where each is drawn. An element
     * moved to inside a ScrollView is scrolled into view.
     *
     * The selected object's components get OnSelect, OnDeselect, OnSubmit and OnCancel (Component; Lua scripts too).
     * Application owns one and updates it each frame after the pointer, before Scene::Update. Main thread only.
     */
    class UINavigator
    {
    public:
        /// Seconds a held direction waits before it repeats, and then between repeats (Unity's input module)
        static constexpr float DefaultRepeatDelay = 0.5f;
        static constexpr float DefaultRepeatInterval = 0.1f;
        /// How far a stick must be pushed to count as a direction
        static constexpr float StickThreshold = 0.5f;

        /// The selected object, or nullptr (none, or it was destroyed or deactivated)
        [[nodiscard]] GameObject *GetSelected() const;
        /// Selects `gameObject` (nullptr: nothing), sending OnDeselect to the one selected before and OnSelect to the new
        /// one. An inactive or destroyed object selects nothing. Selecting the selected object again does nothing.
        void SetSelected(GameObject *gameObject);

        /// Whether Update reads input (default true). The selection and the methods below work either way.
        [[nodiscard]] bool IsEnabled() const { return _enabled; }
        void SetEnabled(const bool enabled) { _enabled = enabled; }
        void SetRepeat(const float delay, const float interval)
        {
            _repeatDelay = delay;
            _repeatInterval = interval;
        }

        /// What an arrow does: changes a selected Slider along its bar, else moves the selection. False when nothing is
        /// selected or there is nowhere to go.
        bool Move(const Scene &scene, const Vector2i &viewport, NavigationDirection direction);
        /// What Tab does (backwards: Shift+Tab): the next selectable element in draw order, wrapping around. False when
        /// nothing is selected or there is no other.
        bool Step(const Scene &scene, const Vector2i &viewport, bool backwards);
        /// Sends OnSubmit to the selected object (a Button clicks)
        void Submit();
        /// Sends OnCancel to the selected object
        void Cancel();

        /// Whether submit or cancel was pressed in the last frame processed, with something selected
        [[nodiscard]] bool WasSubmitPressed() const { return _submitPressed; }
        [[nodiscard]] bool WasCancelPressed() const { return _cancelPressed; }

        /// One frame from the given input, `unscaledDeltaTime` seconds after the last (what Update does; tests call it)
        void Process(const Scene &scene, const Vector2i &viewport, const NavigationInput &input, float unscaledDeltaTime);
        /// One frame from the application's keyboard, gamepads and pointer on the current scene
        void Update();

        /// The selectable elements Tab goes through, in order: those on the overlay canvases, or, with `within` on a
        /// world-space canvas, those on that canvas. Lays the canvases out. Elements with navigation None are left out.
        [[nodiscard]] static std::vector<GameObject *> CollectSelectables(const Scene &scene, const Vector2i &viewport,
                                                                          const GameObject *within = nullptr);
        /// Whether an object is selectable by the rules above (whatever its navigation mode)
        [[nodiscard]] static bool IsSelectable(const GameObject &gameObject);

    private:
        std::weak_ptr<GameObject> _selected;
        bool _enabled = true;
        float _repeatDelay = DefaultRepeatDelay;
        float _repeatInterval = DefaultRepeatInterval;
        // Per held input (up, down, left, right, next, previous): seconds until it fires again; below 0: not held
        std::array<float, 6> _repeat{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
        bool _submitPressed = false;
        bool _cancelPressed = false;
        // The gamepads' submit and cancel buttons last frame, for their presses
        bool _padSubmit = false;
        bool _padCancel = false;

        /// Whether a held input fires this frame: at once, then after the delay, then every interval
        bool Fires(std::size_t index, bool held, float deltaTime);
        /// Selects and scrolls it into view
        void SelectAndShow(const Scene &scene, const Vector2i &viewport, GameObject &gameObject);
        [[nodiscard]] NavigationInput ReadInput();
    };
}
