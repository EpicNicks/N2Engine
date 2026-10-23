#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "WebInputMap.hpp"

// The web player's translation from browser events to the engine's input (web/player/WebInputMap.*). Everything that
// needs a browser (the events themselves, the canvas) is in docs/web-checklist.html; this is the arithmetic and tables.

using namespace N2Engine;
using namespace N2Engine::Web;
using Input::Key;
using Input::MouseButton;

TEST(WebInputMap, LettersDigitsAndFunctionKeysMapToTheirOwnKeys)
{
    EXPECT_EQ(KeyFromDomCode("KeyA"), Key::A);
    EXPECT_EQ(KeyFromDomCode("KeyM"), Key::M);
    EXPECT_EQ(KeyFromDomCode("KeyZ"), Key::Z);
    EXPECT_EQ(KeyFromDomCode("Digit0"), Key::Key0);
    EXPECT_EQ(KeyFromDomCode("Digit9"), Key::Key9);
    EXPECT_EQ(KeyFromDomCode("Numpad0"), Key::Kp0);
    EXPECT_EQ(KeyFromDomCode("Numpad7"), Key::Kp7);
    EXPECT_EQ(KeyFromDomCode("F1"), Key::F1);
    EXPECT_EQ(KeyFromDomCode("F5"), Key::F5);
    EXPECT_EQ(KeyFromDomCode("F12"), Key::F12);
}

TEST(WebInputMap, NamedKeysMap)
{
    EXPECT_EQ(KeyFromDomCode("Space"), Key::Space);
    EXPECT_EQ(KeyFromDomCode("Escape"), Key::Escape);
    EXPECT_EQ(KeyFromDomCode("Enter"), Key::Enter);
    EXPECT_EQ(KeyFromDomCode("NumpadEnter"), Key::KpEnter);
    EXPECT_EQ(KeyFromDomCode("ArrowUp"), Key::Up);
    EXPECT_EQ(KeyFromDomCode("ArrowDown"), Key::Down);
    EXPECT_EQ(KeyFromDomCode("ArrowLeft"), Key::Left);
    EXPECT_EQ(KeyFromDomCode("ArrowRight"), Key::Right);
    EXPECT_EQ(KeyFromDomCode("ShiftLeft"), Key::LeftShift);
    EXPECT_EQ(KeyFromDomCode("ShiftRight"), Key::RightShift);
    EXPECT_EQ(KeyFromDomCode("ControlLeft"), Key::LeftControl);
    EXPECT_EQ(KeyFromDomCode("AltRight"), Key::RightAlt);
    EXPECT_EQ(KeyFromDomCode("MetaLeft"), Key::LeftSuper);
    EXPECT_EQ(KeyFromDomCode("BracketLeft"), Key::LeftBracket);
    EXPECT_EQ(KeyFromDomCode("Backquote"), Key::GraveAccent);
    EXPECT_EQ(KeyFromDomCode("Quote"), Key::Apostrophe);
}

TEST(WebInputMap, UnknownCodesAreUnknown)
{
    EXPECT_EQ(KeyFromDomCode(""), Key::Unknown);
    EXPECT_EQ(KeyFromDomCode("Unidentified"), Key::Unknown);
    EXPECT_EQ(KeyFromDomCode("keya"), Key::Unknown); // case matters: codes are exact
    EXPECT_EQ(KeyFromDomCode("F13"), Key::Unknown);
    EXPECT_EQ(KeyFromDomCode("MediaPlayPause"), Key::Unknown);
}

TEST(WebInputMap, EveryMappedCodeGivesADistinctKey)
{
    // Sweeps the generated runs: no two codes of the runs share a key, so the arithmetic is not off by one
    int seen[200] = {};
    int count = 0;
    auto note = [&](const std::string &code) {
        const Key key = KeyFromDomCode(code);
        ASSERT_NE(key, Key::Unknown) << code;
        ASSERT_LT(static_cast<int>(key), 200);
        EXPECT_EQ(seen[static_cast<int>(key)]++, 0) << code;
        ++count;
    };
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        note(std::string("Key") + c);
    }
    for (char c = '0'; c <= '9'; ++c)
    {
        note(std::string("Digit") + c);
        note(std::string("Numpad") + c);
    }
    for (int i = 1; i <= 12; ++i)
    {
        note("F" + std::to_string(i));
    }
    EXPECT_EQ(count, 26 + 20 + 12);
}

TEST(WebInputMap, MouseButtonsSwapRightAndMiddle)
{
    EXPECT_EQ(MouseButtonFromDom(0), MouseButton::Left);
    EXPECT_EQ(MouseButtonFromDom(1), MouseButton::Middle);
    EXPECT_EQ(MouseButtonFromDom(2), MouseButton::Right);
    EXPECT_EQ(MouseButtonFromDom(3), MouseButton::Button4);
    EXPECT_EQ(MouseButtonFromDom(4), MouseButton::Button5);
    EXPECT_FALSE(MouseButtonFromDom(5).has_value());
    EXPECT_FALSE(MouseButtonFromDom(-1).has_value());
}

TEST(WebInputMap, OnlyPageScrollingKeysAreSuppressed)
{
    EXPECT_TRUE(SuppressesBrowserDefault(Key::Space));
    EXPECT_TRUE(SuppressesBrowserDefault(Key::Up));
    EXPECT_TRUE(SuppressesBrowserDefault(Key::Tab));
    EXPECT_TRUE(SuppressesBrowserDefault(Key::Backspace));
    EXPECT_FALSE(SuppressesBrowserDefault(Key::A));
    EXPECT_FALSE(SuppressesBrowserDefault(Key::F5));
    EXPECT_FALSE(SuppressesBrowserDefault(Key::F12));
    EXPECT_FALSE(SuppressesBrowserDefault(Key::Escape));
    EXPECT_FALSE(SuppressesBrowserDefault(Key::LeftControl));
}

TEST(WebInputMap, PointerCoordinatesScaleFromTheCanvasToTheRenderSize)
{
    // A canvas shown 800x450 CSS pixels at (100, 50), rendered at 400x225: every CSS pixel is half a render pixel
    EXPECT_FLOAT_EQ(ClientToRender(100.0, 100.0, 800.0, 400), 0.0f);
    EXPECT_FLOAT_EQ(ClientToRender(500.0, 100.0, 800.0, 400), 200.0f);
    EXPECT_FLOAT_EQ(ClientToRender(900.0, 100.0, 800.0, 400), 400.0f);
    EXPECT_FLOAT_EQ(ClientToRender(50.0, 50.0, 450.0, 225), 0.0f);
    EXPECT_FLOAT_EQ(ClientToRender(275.0, 50.0, 450.0, 225), 112.5f);
    // Not clamped: a pointer off the canvas reads outside it
    EXPECT_LT(ClientToRender(0.0, 100.0, 800.0, 400), 0.0f);
    EXPECT_GT(ClientToRender(1000.0, 100.0, 800.0, 400), 400.0f);
    // No canvas size, no position
    EXPECT_FLOAT_EQ(ClientToRender(300.0, 100.0, 0.0, 400), 0.0f);
}

TEST(WebInputMap, WheelNotchesAreOneUnitWhateverTheMode)
{
    // Chrome: a notch is 100 pixels, down is positive; the engine's up is positive
    EXPECT_FLOAT_EQ(WheelToScroll(100.0, 0, true), -1.0f);
    EXPECT_FLOAT_EQ(WheelToScroll(-100.0, 0, true), 1.0f);
    EXPECT_FLOAT_EQ(WheelToScroll(50.0, 0, true), -0.5f);
    // Firefox: a notch is 3 lines
    EXPECT_FLOAT_EQ(WheelToScroll(3.0, 1, true), -1.0f);
    // A page is a notch
    EXPECT_FLOAT_EQ(WheelToScroll(1.0, 2, true), -1.0f);
    // The horizontal axis is not inverted
    EXPECT_FLOAT_EQ(WheelToScroll(100.0, 0, false), 1.0f);
}

TEST(WebInputMap, RenderSizeKeepsTheShapeAndCapsTheWidth)
{
    EXPECT_EQ(FitRenderSize(800.0, 450.0, 1.0, 1280), (RenderSize{800, 450}));
    EXPECT_EQ(FitRenderSize(1280.0, 720.0, 1.0, 1280), (RenderSize{1280, 720}));
    EXPECT_EQ(FitRenderSize(2560.0, 1440.0, 1.0, 1280), (RenderSize{1280, 720}));
    EXPECT_EQ(FitRenderSize(1920.0, 1080.0, 1.0, 1280), (RenderSize{1280, 720}));
    EXPECT_EQ(FitRenderSize(1500.5, 700.0, 1.0, 1280), (RenderSize{1280, 597}));
    // Never below one pixel in a dimension; nothing at all for a canvas with no size
    EXPECT_EQ(FitRenderSize(5000.0, 1.0, 1.0, 1280), (RenderSize{1280, 1}));
    EXPECT_EQ(FitRenderSize(0.0, 450.0, 1.0, 1280), (RenderSize{}));
    EXPECT_EQ(FitRenderSize(800.0, 0.0, 1.0, 1280), (RenderSize{}));
    EXPECT_EQ(FitRenderSize(-5.0, 450.0, 1.0, 1280), (RenderSize{}));
}

TEST(WebInputMap, RenderSizeFollowsTheDevicePixelRatioUpToTheCap)
{
    // HiDPI: device resolution, so the browser does not stretch the frame
    EXPECT_EQ(FitRenderSize(800.0, 450.0, 1.5, 1280), (RenderSize{1200, 675}));
    EXPECT_EQ(FitRenderSize(640.0, 360.0, 2.0, 1280), (RenderSize{1280, 720}));
    // Past the cap: capped, same shape
    EXPECT_EQ(FitRenderSize(960.0, 600.0, 1.5, 1280), (RenderSize{1280, 800}));
    EXPECT_EQ(FitRenderSize(1920.0, 1080.0, 2.0, 1280), (RenderSize{1280, 720}));
    // A tall canvas (a phone held upright, ratio 3) is capped by area too: 1170x2532 would be three times 1280x720
    EXPECT_EQ(FitRenderSize(390.0, 844.0, 3.0, 1280), (RenderSize{688, 1489}));
    // An absurd ratio is clamped to 16
    EXPECT_EQ(FitRenderSize(800.0, 450.0, 1e308, 1280), (RenderSize{1280, 720}));
    // Zoomed out (a ratio below 1) renders smaller, in proportion
    EXPECT_EQ(FitRenderSize(800.0, 400.0, 0.5, 1280), (RenderSize{400, 200}));
    // A ratio that is not a positive number counts as 1; a canvas with no size is still nothing
    EXPECT_EQ(FitRenderSize(800.0, 450.0, 0.0, 1280), (RenderSize{800, 450}));
    EXPECT_EQ(FitRenderSize(800.0, 450.0, -2.0, 1280), (RenderSize{800, 450}));
    EXPECT_EQ(FitRenderSize(800.0, 450.0, std::numeric_limits<double>::quiet_NaN(), 1280), (RenderSize{800, 450}));
    EXPECT_EQ(FitRenderSize(0.0, 450.0, 2.0, 1280), (RenderSize{}));
}
