#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "engine/input/InputDevice.hpp"
#include "engine/input/InputTypes.hpp"

// The pure part of the web player's input: how a browser's keyboard, mouse, wheel and gamepad readings become the engine's own
// types. No Emscripten and no DOM in here, so the engine's tests can run it under node (tests/web).
namespace N2Engine::Web
{
    /// The engine's key for a DOM KeyboardEvent.code ("KeyA", "ArrowLeft", "Numpad1", ...), which names the physical
    /// key (layout independent, like GLFW's key tokens). Key::Unknown for a code the engine has no key for.
    [[nodiscard]] Input::Key KeyFromDomCode(std::string_view code);

    /// The engine's button for a DOM MouseEvent.button (0 main, 1 auxiliary/middle, 2 secondary/right, 3 back, 4
    /// forward). The DOM numbers right and middle the other way round to GLFW and the engine (0 left, 1 right,
    /// 2 middle). Empty for a number the engine has no button for.
    [[nodiscard]] std::optional<Input::MouseButton> MouseButtonFromDom(int domButton);

    /// True for the keys whose browser default the player cancels while the game runs (page scroll, focus move, history
    /// back), so the game can use them. Every other key, F5, F11, F12 and the Ctrl/Alt/Meta shortcuts among them, keeps
    /// its browser meaning.
    [[nodiscard]] bool SuppressesBrowserDefault(Input::Key key);

    /// The text a keydown types, from its KeyboardEvent.key: the key itself when it is one character ("a", "A", "é",
    /// " "), nothing for a named key ("Enter", "Shift", "ArrowUp", "Dead") or while Ctrl or Meta is held (a shortcut).
    /// The browser's stand-in for GLFW's character callback, which the web build does not have.
    [[nodiscard]] std::string_view TypedTextFromKey(std::string_view key, bool ctrlOrMeta);

    /// A pointer coordinate in render pixels: `client` is the event's viewport coordinate, `origin` the canvas's edge on
    /// that axis, `cssExtent` the canvas's size on that axis as shown (CSS pixels) and `renderExtent` the size the game
    /// renders at on it. 0 when the canvas has no size. Not clamped: a pointer dragged off the canvas reads outside it.
    [[nodiscard]] float ClientToRender(double client, double origin, double cssExtent, int renderExtent);

    /// A wheel delta as the engine's scroll units (one notch of a mouse wheel is 1), from DOM WheelEvent.deltaX/deltaY
    /// and deltaMode (0 pixels, 1 lines, 2 pages). `invert` flips the sign: the DOM's vertical delta is positive
    /// scrolling down, the engine's (GLFW's) is positive scrolling up; the horizontal one agrees in both.
    [[nodiscard]] float WheelToScroll(double delta, int domDeltaMode, bool invert);

    /// One button of a DOM Gamepad: GamepadButton.pressed and .value (0..1)
    struct DomGamepadButton
    {
        bool pressed = false;
        double value = 0.0;
    };

    /// True for a DOM Gamepad.mapping the player reads: "standard" (the W3C standard gamepad layout, which the browser
    /// remaps known pads to). Any other mapping ("" for a pad the browser does not know) is a joystick without a gamepad
    /// mapping, as GLFW reports one it has no mapping for: present, but no state, so no binding reads it.
    [[nodiscard]] bool IsStandardGamepadMapping(std::string_view mapping);

    /// The engine's state for a gamepad in the W3C standard layout, from its DOM Gamepad.axes and .buttons. Buttons 0-3
    /// are South, East, West, North; 4, 5 the bumpers; 6, 7 the analog triggers (their values, 0..1, become the engine's
    /// -1 at rest to +1 pulled); 8 Back, 9 Start, 10, 11 the stick clicks; 12-15 the D-pad up, down, left, right; 16 Guide.
    /// Axes 0-3 are the left and right sticks' x and y, y down positive (as GLFW and the engine have it). Values past
    /// -1..1 are clamped; a button or axis the pad does not have reads released (a trigger at rest).
    [[nodiscard]] Input::GamepadState GamepadStateFromStandard(std::span<const double> axes,
                                                               std::span<const DomGamepadButton> buttons);

    struct RenderSize
    {
        int width = 0;
        int height = 0;
        friend bool operator==(const RenderSize &, const RenderSize &) = default;
    };

    /// The size the game renders at for a canvas shown at cssWidth x cssHeight CSS pixels on a screen with
    /// devicePixelRatio device pixels per CSS pixel: the same shape, at device resolution (so the browser does not
    /// stretch, and blur, the frame) but at most maxWidth wide and maxWidth x maxWidth * 5/8 pixels in all, so a tall canvas
    /// costs no more than a wide one (a software renderer's cost is its pixel count; past the caps the browser scales the
    /// canvas up). A devicePixelRatio that is not a positive number counts as 1, and one above 16 as 16.
    /// {0, 0} when the canvas has no size (hidden, or not laid out yet).
    [[nodiscard]] RenderSize FitRenderSize(double cssWidth, double cssHeight, double devicePixelRatio, int maxWidth);
}
