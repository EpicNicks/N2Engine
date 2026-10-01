#pragma once

#include <math/Vector2.hpp>

struct GLFWwindow;

namespace N2Engine::Input
{
    class Mouse
    {
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

        // Static callback for GLFW
        static void ScrollCallback(GLFWwindow *window, double xOffset, double yOffset);

    public:
        explicit Mouse(GLFWwindow *window);
        ~Mouse();

        static Mouse* Get();

        // Called at start of frame to clear previous frame's deltas and calculate new ones
        void Update();

        // Accessors
        [[nodiscard]] Math::Vector2 GetScrollDelta() const { return _scrollDelta; }
        [[nodiscard]] Math::Vector2 GetPositionDelta() const { return _positionDelta; }
        [[nodiscard]] Math::Vector2 GetPosition() const { return _currentPosition; }

        // Allow ScrollCallback to access private members
        void AccumulateScroll(float xOffset, float yOffset);
    };
}
