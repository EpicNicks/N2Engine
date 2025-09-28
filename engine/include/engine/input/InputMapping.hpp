#pragma once
#include <unordered_map>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace N2Engine
{
    namespace Input
    {
        enum class Key
        {
            Unknown = 0,

            // Printable keys
            Space,
            Apostrophe,
            Comma,
            Minus,
            Period,
            Slash,
            Key0,
            Key1,
            Key2,
            Key3,
            Key4,
            Key5,
            Key6,
            Key7,
            Key8,
            Key9,
            Semicolon,
            Equal,
            A,
            B,
            C,
            D,
            E,
            F,
            G,
            H,
            I,
            J,
            K,
            L,
            M,
            N,
            O,
            P,
            Q,
            R,
            S,
            T,
            U,
            V,
            W,
            X,
            Y,
            Z,
            LeftBracket,
            Backslash,
            RightBracket,
            GraveAccent,

            // Function keys
            Escape,
            Enter,
            Tab,
            Backspace,
            Insert,
            Delete,
            Right,
            Left,
            Down,
            Up,
            PageUp,
            PageDown,
            Home,
            End,
            CapsLock,
            ScrollLock,
            NumLock,
            PrintScreen,
            Pause,
            F1,
            F2,
            F3,
            F4,
            F5,
            F6,
            F7,
            F8,
            F9,
            F10,
            F11,
            F12,

            // Keypad
            Kp0,
            Kp1,
            Kp2,
            Kp3,
            Kp4,
            Kp5,
            Kp6,
            Kp7,
            Kp8,
            Kp9,
            KpDecimal,
            KpDivide,
            KpMultiply,
            KpSubtract,
            KpAdd,
            KpEnter,
            KpEqual,

            // Modifiers
            LeftShift,
            LeftControl,
            LeftAlt,
            LeftSuper,
            RightShift,
            RightControl,
            RightAlt,
            RightSuper,
            Menu
        };

        enum class GamepadButton
        {
            // Face buttons (using cardinal directions for platform agnostic naming)
            South, // A on Xbox, Cross on PlayStation, B on Nintendo
            East,  // B on Xbox, Circle on PlayStation, A on Nintendo
            West,  // X on Xbox, Square on PlayStation, Y on Nintendo
            North, // Y on Xbox, Triangle on PlayStation, X on Nintendo

            // Shoulder buttons
            LeftBumper,
            RightBumper,

            // Back/Start buttons
            Back,
            Start,
            Guide,

            // Stick buttons
            LeftThumb,
            RightThumb,

            // D-pad
            DpadUp,
            DpadRight,
            DpadDown,
            DpadLeft
        };

        enum class GamepadAxis
        {
            LeftX,
            LeftY, // Left stick
            RightX,
            RightY, // Right stick
            LeftTrigger,
            RightTrigger
        };

        enum class MouseButton
        {
            Left,
            Right,
            Middle,
            Button4,
            Button5,
            Button6,
            Button7,
            Button8
        };

        // Mapping tables for GLFW
        static const std::unordered_map<Key, int> KeyToGLFW = {
            {Key::Unknown, GLFW_KEY_UNKNOWN},

            // Printable keys
            {Key::Space, GLFW_KEY_SPACE},
            {Key::Apostrophe, GLFW_KEY_APOSTROPHE},
            {Key::Comma, GLFW_KEY_COMMA},
            {Key::Minus, GLFW_KEY_MINUS},
            {Key::Period, GLFW_KEY_PERIOD},
            {Key::Slash, GLFW_KEY_SLASH},
            {Key::Key0, GLFW_KEY_0},
            {Key::Key1, GLFW_KEY_1},
            {Key::Key2, GLFW_KEY_2},
            {Key::Key3, GLFW_KEY_3},
            {Key::Key4, GLFW_KEY_4},
            {Key::Key5, GLFW_KEY_5},
            {Key::Key6, GLFW_KEY_6},
            {Key::Key7, GLFW_KEY_7},
            {Key::Key8, GLFW_KEY_8},
            {Key::Key9, GLFW_KEY_9},
            {Key::Semicolon, GLFW_KEY_SEMICOLON},
            {Key::Equal, GLFW_KEY_EQUAL},
            {Key::A, GLFW_KEY_A},
            {Key::B, GLFW_KEY_B},
            {Key::C, GLFW_KEY_C},
            {Key::D, GLFW_KEY_D},
            {Key::E, GLFW_KEY_E},
            {Key::F, GLFW_KEY_F},
            {Key::G, GLFW_KEY_G},
            {Key::H, GLFW_KEY_H},
            {Key::I, GLFW_KEY_I},
            {Key::J, GLFW_KEY_J},
            {Key::K, GLFW_KEY_K},
            {Key::L, GLFW_KEY_L},
            {Key::M, GLFW_KEY_M},
            {Key::N, GLFW_KEY_N},
            {Key::O, GLFW_KEY_O},
            {Key::P, GLFW_KEY_P},
            {Key::Q, GLFW_KEY_Q},
            {Key::R, GLFW_KEY_R},
            {Key::S, GLFW_KEY_S},
            {Key::T, GLFW_KEY_T},
            {Key::U, GLFW_KEY_U},
            {Key::V, GLFW_KEY_V},
            {Key::W, GLFW_KEY_W},
            {Key::X, GLFW_KEY_X},
            {Key::Y, GLFW_KEY_Y},
            {Key::Z, GLFW_KEY_Z},
            {Key::LeftBracket, GLFW_KEY_LEFT_BRACKET},
            {Key::Backslash, GLFW_KEY_BACKSLASH},
            {Key::RightBracket, GLFW_KEY_RIGHT_BRACKET},
            {Key::GraveAccent, GLFW_KEY_GRAVE_ACCENT},

            // Function keys
            {Key::Escape, GLFW_KEY_ESCAPE},
            {Key::Enter, GLFW_KEY_ENTER},
            {Key::Tab, GLFW_KEY_TAB},
            {Key::Backspace, GLFW_KEY_BACKSPACE},
            {Key::Insert, GLFW_KEY_INSERT},
            {Key::Delete, GLFW_KEY_DELETE},
            {Key::Right, GLFW_KEY_RIGHT},
            {Key::Left, GLFW_KEY_LEFT},
            {Key::Down, GLFW_KEY_DOWN},
            {Key::Up, GLFW_KEY_UP},
            {Key::PageUp, GLFW_KEY_PAGE_UP},
            {Key::PageDown, GLFW_KEY_PAGE_DOWN},
            {Key::Home, GLFW_KEY_HOME},
            {Key::End, GLFW_KEY_END},
            {Key::CapsLock, GLFW_KEY_CAPS_LOCK},
            {Key::ScrollLock, GLFW_KEY_SCROLL_LOCK},
            {Key::NumLock, GLFW_KEY_NUM_LOCK},
            {Key::PrintScreen, GLFW_KEY_PRINT_SCREEN},
            {Key::Pause, GLFW_KEY_PAUSE},
            {Key::F1, GLFW_KEY_F1},
            {Key::F2, GLFW_KEY_F2},
            {Key::F3, GLFW_KEY_F3},
            {Key::F4, GLFW_KEY_F4},
            {Key::F5, GLFW_KEY_F5},
            {Key::F6, GLFW_KEY_F6},
            {Key::F7, GLFW_KEY_F7},
            {Key::F8, GLFW_KEY_F8},
            {Key::F9, GLFW_KEY_F9},
            {Key::F10, GLFW_KEY_F10},
            {Key::F11, GLFW_KEY_F11},
            {Key::F12, GLFW_KEY_F12},

            // Keypad
            {Key::Kp0, GLFW_KEY_KP_0},
            {Key::Kp1, GLFW_KEY_KP_1},
            {Key::Kp2, GLFW_KEY_KP_2},
            {Key::Kp3, GLFW_KEY_KP_3},
            {Key::Kp4, GLFW_KEY_KP_4},
            {Key::Kp5, GLFW_KEY_KP_5},
            {Key::Kp6, GLFW_KEY_KP_6},
            {Key::Kp7, GLFW_KEY_KP_7},
            {Key::Kp8, GLFW_KEY_KP_8},
            {Key::Kp9, GLFW_KEY_KP_9},
            {Key::KpDecimal, GLFW_KEY_KP_DECIMAL},
            {Key::KpDivide, GLFW_KEY_KP_DIVIDE},
            {Key::KpMultiply, GLFW_KEY_KP_MULTIPLY},
            {Key::KpSubtract, GLFW_KEY_KP_SUBTRACT},
            {Key::KpAdd, GLFW_KEY_KP_ADD},
            {Key::KpEnter, GLFW_KEY_KP_ENTER},
            {Key::KpEqual, GLFW_KEY_KP_EQUAL},

            // Modifiers
            {Key::LeftShift, GLFW_KEY_LEFT_SHIFT},
            {Key::LeftControl, GLFW_KEY_LEFT_CONTROL},
            {Key::LeftAlt, GLFW_KEY_LEFT_ALT},
            {Key::LeftSuper, GLFW_KEY_LEFT_SUPER},
            {Key::RightShift, GLFW_KEY_RIGHT_SHIFT},
            {Key::RightControl, GLFW_KEY_RIGHT_CONTROL},
            {Key::RightAlt, GLFW_KEY_RIGHT_ALT},
            {Key::RightSuper, GLFW_KEY_RIGHT_SUPER},
            {Key::Menu, GLFW_KEY_MENU}};

        static const std::unordered_map<GamepadButton, int> GamepadButtonToGLFW = {
            {GamepadButton::South, GLFW_GAMEPAD_BUTTON_A},
            {GamepadButton::East, GLFW_GAMEPAD_BUTTON_B},
            {GamepadButton::West, GLFW_GAMEPAD_BUTTON_X},
            {GamepadButton::North, GLFW_GAMEPAD_BUTTON_Y},
            {GamepadButton::LeftBumper, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER},
            {GamepadButton::RightBumper, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER},
            {GamepadButton::Back, GLFW_GAMEPAD_BUTTON_BACK},
            {GamepadButton::Start, GLFW_GAMEPAD_BUTTON_START},
            {GamepadButton::Guide, GLFW_GAMEPAD_BUTTON_GUIDE},
            {GamepadButton::LeftThumb, GLFW_GAMEPAD_BUTTON_LEFT_THUMB},
            {GamepadButton::RightThumb, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB},
            {GamepadButton::DpadUp, GLFW_GAMEPAD_BUTTON_DPAD_UP},
            {GamepadButton::DpadRight, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT},
            {GamepadButton::DpadDown, GLFW_GAMEPAD_BUTTON_DPAD_DOWN},
            {GamepadButton::DpadLeft, GLFW_GAMEPAD_BUTTON_DPAD_LEFT}};

        static const std::unordered_map<GamepadAxis, int> GamepadAxisToGLFW = {
            {GamepadAxis::LeftX, GLFW_GAMEPAD_AXIS_LEFT_X},
            {GamepadAxis::LeftY, GLFW_GAMEPAD_AXIS_LEFT_Y},
            {GamepadAxis::RightX, GLFW_GAMEPAD_AXIS_RIGHT_X},
            {GamepadAxis::RightY, GLFW_GAMEPAD_AXIS_RIGHT_Y},
            {GamepadAxis::LeftTrigger, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER},
            {GamepadAxis::RightTrigger, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER}};

        static const std::unordered_map<MouseButton, int> MouseButtonToGLFW = {
            {MouseButton::Left, GLFW_MOUSE_BUTTON_LEFT},
            {MouseButton::Right, GLFW_MOUSE_BUTTON_RIGHT},
            {MouseButton::Middle, GLFW_MOUSE_BUTTON_MIDDLE},
            {MouseButton::Button4, GLFW_MOUSE_BUTTON_4},
            {MouseButton::Button5, GLFW_MOUSE_BUTTON_5},
            {MouseButton::Button6, GLFW_MOUSE_BUTTON_6},
            {MouseButton::Button7, GLFW_MOUSE_BUTTON_7},
            {MouseButton::Button8, GLFW_MOUSE_BUTTON_8}};

        // Reverse lookup tables (GLFW -> Engine)
        static const std::unordered_map<int, Key> GLFWToKey = []()
        {
            std::unordered_map<int, Key> reverse;
            for (const auto &pair : KeyToGLFW)
            {
                reverse[pair.second] = pair.first;
            }
            return reverse;
        }();

        static const std::unordered_map<int, GamepadButton> GLFWToGamepadButton = []()
        {
            std::unordered_map<int, GamepadButton> reverse;
            for (const auto &pair : GamepadButtonToGLFW)
            {
                reverse[pair.second] = pair.first;
            }
            return reverse;
        }();

        static const std::unordered_map<int, MouseButton> GLFWToMouseButton = []()
        {
            std::unordered_map<int, MouseButton> reverse;
            for (const auto &pair : MouseButtonToGLFW)
            {
                reverse[pair.second] = pair.first;
            }
            return reverse;
        }();
    }
}