#pragma once

#include <optional>
#include <string_view>

#include "engine/input/InputTypes.hpp"

// The pure part of the web player's input: how a browser's keyboard, mouse and wheel events become the engine's own
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

    /// A pointer coordinate in render pixels: `client` is the event's viewport coordinate, `origin` the canvas's edge on
    /// that axis, `cssExtent` the canvas's size on that axis as shown (CSS pixels) and `renderExtent` the size the game
    /// renders at on it. 0 when the canvas has no size. Not clamped: a pointer dragged off the canvas reads outside it.
    [[nodiscard]] float ClientToRender(double client, double origin, double cssExtent, int renderExtent);

    /// A wheel delta as the engine's scroll units (one notch of a mouse wheel is 1), from DOM WheelEvent.deltaX/deltaY
    /// and deltaMode (0 pixels, 1 lines, 2 pages). `invert` flips the sign: the DOM's vertical delta is positive
    /// scrolling down, the engine's (GLFW's) is positive scrolling up; the horizontal one agrees in both.
    [[nodiscard]] float WheelToScroll(double delta, int domDeltaMode, bool invert);

    struct RenderSize
    {
        int width = 0;
        int height = 0;
        friend bool operator==(const RenderSize &, const RenderSize &) = default;
    };

    /// The size the game renders at for a canvas shown at cssWidth x cssHeight CSS pixels: the same shape, at most
    /// maxWidth wide (a software renderer's cost is its pixel count; the browser scales the canvas up to its CSS size).
    /// {0, 0} when the canvas has no size (hidden, or not laid out yet).
    [[nodiscard]] RenderSize FitRenderSize(double cssWidth, double cssHeight, int maxWidth);
}
