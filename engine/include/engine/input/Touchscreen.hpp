#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <math/Vector2.hpp>

namespace N2Engine::Input
{
    /// Unity's TouchPhase
    enum class TouchPhase : std::uint8_t
    {
        Began,      ///< the finger touched down this frame
        Moved,      ///< it moved since the previous frame
        Stationary, ///< it is down and did not move
        Ended,      ///< it was lifted (reported for one frame)
        Canceled,   ///< the system took it away (reported for one frame)
    };

    [[nodiscard]] std::string_view TouchPhaseName(TouchPhase phase);

    /// One finger: an id that stays the same while it is down, its position in render pixels (top-left origin, y down,
    /// as Mouse::GetPosition), and its phase this frame
    struct Touch
    {
        int id = 0;
        Math::Vector2 position{0.0f, 0.0f};
        TouchPhase phase = TouchPhase::Began;
    };

    /**
     * The touches on the screen, as Unity's Input.touches. A host feeds finger events (the web player: the canvas's
     * touch events; tests) with Down/Up/Cancel at any time; InputSystem::Update publishes them once per frame, so a
     * frame's touches stay the same while scripts read them. GLFW has no touch input, so on desktop the list is empty
     * unless something injects.
     *
     * A finger put down and lifted between two frames is reported Began in one frame and Ended in the next, so no
     * tap is lost. At most MaxTouches fingers are tracked; more are ignored until one lifts.
     *
     * Main thread only.
     */
    class Touchscreen
    {
    public:
        static constexpr std::size_t MaxTouches = 10;

        static Touchscreen &Get();

        /// A finger is down at `position` (a new id begins a touch; a known one moves)
        void Down(int id, const Math::Vector2 &position);
        /// The finger is lifted (at `position`)
        void Up(int id, const Math::Vector2 &position);
        /// The system took the finger away
        void Cancel(int id);
        /// Every finger lifted (focus lost)
        void CancelAll();

        /// Publishes the fingers' state for this frame
        void Update();

        [[nodiscard]] const std::vector<Touch> &GetTouches() const { return _touches; }
        [[nodiscard]] std::size_t GetTouchCount() const { return _touches.size(); }

        /// Forgets everything (tests)
        void Reset();

    private:
        struct Finger
        {
            int id = 0;
            Math::Vector2 position{0.0f, 0.0f};
            Math::Vector2 published{0.0f, 0.0f};
            bool beganPending = true;  ///< not yet reported Began
            bool ended = false;        ///< lifted or canceled, to be reported once
            bool canceled = false;
        };
        std::vector<Finger> _fingers;
        std::vector<Touch> _touches;

        Finger *Find(int id);
    };
}
