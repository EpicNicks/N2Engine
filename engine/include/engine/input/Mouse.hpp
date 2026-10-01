#pragma once

#include <cstdint>
#include <optional>

#include <math/Vector2.hpp>

struct GLFWwindow;

namespace N2Engine::Input
{
    enum class MouseButton; // InputTypes.hpp: Left = 0, Right = 1, Middle = 2, Button4..Button8 = 3..7

    class Mouse
    {
    public:
        /// Buttons 0..7 (GLFW's eight): 0 left, 1 right, 2 middle, as in Unity's GetMouseButton
        static constexpr int ButtonCount = 8;

        /// The bit for a button in a button mask (InjectPointer); 0 for a button outside 0..7
        [[nodiscard]] static constexpr uint32_t ButtonBit(const int button)
        {
            return button >= 0 && button < ButtonCount ? 1u << button : 0u;
        }

    private:
        GLFWwindow *_window;

        // Per-frame state. Scroll arrives in callbacks during glfwPollEvents, before Update, so it's
        // gathered in _pendingScroll and published to _scrollDelta by Update (which used to zero it
        // straight after the events arrived, so GetScrollDelta was always zero)
        Math::Vector2 _pendingScroll{0.0f, 0.0f};
        Math::Vector2 _scrollDelta{0.0f, 0.0f};
        Math::Vector2 _lastPosition{0.0f, 0.0f};
        Math::Vector2 _positionDelta{0.0f, 0.0f};
        Math::Vector2 _currentPosition{0.0f, 0.0f};

        // Button masks (bit n = button n held) as of this Update and the one before, for the edges
        uint32_t _buttons = 0;
        uint32_t _previousButtons = 0;

        // A pointer state handed in by InjectPointer, used by the next Update instead of the device
        struct InjectedPointer
        {
            Math::Vector2 position;
            uint32_t buttons;
        };
        std::optional<InjectedPointer> _injected;

        // Static callback for GLFW
        static void ScrollCallback(GLFWwindow *window, double xOffset, double yOffset);

    public:
        explicit Mouse(GLFWwindow *window);
        ~Mouse();

        static Mouse* Get();

        /// Called once per frame, at its start (InputSystem::Update, right after glfwPollEvents): publishes
        /// the scroll that arrived since the last Update and samples the cursor position and the buttons.
        /// Without a window (and nothing injected) position and buttons keep their last values.
        void Update();

        // Accessors
        [[nodiscard]] Math::Vector2 GetScrollDelta() const { return _scrollDelta; }
        [[nodiscard]] Math::Vector2 GetPositionDelta() const { return _positionDelta; }
        /// Window coordinates: origin at the top-left, y down (the space Camera::ScreenPointToRay takes)
        [[nodiscard]] Math::Vector2 GetPosition() const { return _currentPosition; }

        /// Whether the button is held, as of the last Update. False for a button outside 0..7.
        [[nodiscard]] bool GetButton(int button) const;
        /// Whether the button went down in the last Update (held now, not in the Update before)
        [[nodiscard]] bool GetButtonDown(int button) const;
        /// Whether the button was released in the last Update (held in the Update before, not now)
        [[nodiscard]] bool GetButtonUp(int button) const;
        [[nodiscard]] bool GetButton(MouseButton button) const;
        [[nodiscard]] bool GetButtonDown(MouseButton button) const;
        [[nodiscard]] bool GetButtonUp(MouseButton button) const;

        // Allow ScrollCallback to access private members
        void AccumulateScroll(float xOffset, float yOffset);

        /// Test seam (like AccumulateScroll): the next Update reads this position and button mask (ButtonBit)
        /// instead of the device, so tests can drive the pointer without a window. One call covers one Update;
        /// later Updates sample the device again, or, without a window, keep the injected state.
        void InjectPointer(const Math::Vector2 &position, uint32_t buttons);
    };
}
