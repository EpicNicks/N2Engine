#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/InputValue.hpp"

#include <GLFW/glfw3.h>

// Window's no-window path (#74): a headless application on the software renderer gets a renderer and an input
// system but no GLFW window, so it runs (and renders real pixels) with no display or GPU, as on the CI runner.

using namespace N2Engine;
using namespace N2Engine::Input;
using RenderBackend = Config::ApplicationOptions::RenderBackend;

namespace
{
    Config::ApplicationOptions Options(const RenderBackend backend, const bool headless)
    {
        Config::ApplicationOptions options;
        options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
        options.renderBackend = backend;
        options.isHeadless = headless;
        return options;
    }

    /// Collects every GLFW error while a test runs, and expects none: the no-window path must never call GLFW
    /// (which, never initialised, would report GLFW_NOT_INITIALIZED through this callback)
    class WindowlessWindowTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Errors().clear();
            _previous = glfwSetErrorCallback(&Collect);
        }

        void TearDown() override
        {
            glfwSetErrorCallback(_previous);
            for (const auto &[code, description] : Errors())
            {
                ADD_FAILURE() << "GLFW error 0x" << std::hex << code << std::dec << ": " << description
                              << (code == GLFW_NOT_INITIALIZED ? " (GLFW_NOT_INITIALIZED)" : "");
            }
        }

    private:
        static std::vector<std::pair<int, std::string>> &Errors()
        {
            static std::vector<std::pair<int, std::string>> errors;
            return errors;
        }

        static void Collect(const int code, const char *description)
        {
            Errors().emplace_back(code, description != nullptr ? description : "");
        }

        GLFWerrorfun _previous = nullptr;
    };
}

TEST_F(WindowlessWindowTest, OnlyAHeadlessSoftwareApplicationHasNoWindow)
{
    EXPECT_TRUE(Window::UsesNoWindow(Options(RenderBackend::SOFTWARE, true)));

    // A visible software window presents its frames; the GPU backends need a window for their context
    EXPECT_FALSE(Window::UsesNoWindow(Options(RenderBackend::SOFTWARE, false)));
    EXPECT_FALSE(Window::UsesNoWindow(Options(RenderBackend::OPENGL, true)));
    EXPECT_FALSE(Window::UsesNoWindow(Options(RenderBackend::VULKAN, true)));
    EXPECT_FALSE(Window::UsesNoWindow(Options(RenderBackend::OPENGL, false)));
}

TEST_F(WindowlessWindowTest, InitCreatesARendererAndInputButNoWindow)
{
    Window window;
    ASSERT_TRUE(window.InitWindow(Options(RenderBackend::SOFTWARE, true))) << window.GetInitError();

    EXPECT_TRUE(window.IsValid());
    EXPECT_TRUE(window.IsWindowless());
    EXPECT_FALSE(window.RendererFailed());
    EXPECT_TRUE(window.GetInitError().empty());
    EXPECT_NE(window.GetRenderer(), nullptr);
    EXPECT_NE(window.GetInputSystem(), nullptr);
    EXPECT_FALSE(window.ShouldClose()) << "nothing can close a window that doesn't exist";

    // The size the renderer started at stands in for the window's
    EXPECT_EQ(window.GetWindowDimensions()[0], Window::FallbackWidth);
    EXPECT_EQ(window.GetWindowDimensions()[1], Window::FallbackHeight);
    EXPECT_EQ(window.GetRenderDimensions()[0], Window::FallbackWidth);
    EXPECT_EQ(window.GetRenderDimensions()[1], Window::FallbackHeight);

    // None of these may reach GLFW, which was never initialised (TearDown fails on any GLFW error)
    EXPECT_FALSE(Window::HasGlfw());
    window.PollEvents();
    window.SetTitle("Windowless");
    EXPECT_EQ(window.GetTitle(), "Windowless");
    window.SetWindowMode(WindowMode::Fullscreen);
    EXPECT_FALSE(window.ShouldClose());

    // Gamepads read nothing, without asking GLFW
    EXPECT_TRUE(InputSystem::GetConnectedGamepads().empty());
    AxisBinding axis(window, GamepadAxis::LeftX);
    (void)axis.getValue();
    GamepadButtonBinding button(window, GamepadButton::South);
    (void)button.getValue();
    GamepadStickBinding stick(window, GamepadAxis::LeftX, GamepadAxis::LeftY);
    (void)stick.getValue();

    window.Shutdown();
}

TEST_F(WindowlessWindowTest, RendersAndReadsBackAtTheRenderSize)
{
    Window window;
    ASSERT_TRUE(window.InitWindow(Options(RenderBackend::SOFTWARE, true))) << window.GetInitError();

    // As the editor server's SetViewportSize does it
    constexpr int width = 8;
    constexpr int height = 4;
    ASSERT_TRUE(window.SetRenderSize(width, height));
    EXPECT_EQ(window.GetRenderDimensions()[0], width);
    EXPECT_EQ(window.GetRenderDimensions()[1], height);

    // A frame as Application::Render draws one, with nothing in it: the clear colour everywhere
    window.clearColor = Common::Color::Red;
    window.Clear();
    auto *renderer = window.GetRenderer();
    ASSERT_NE(renderer, nullptr);
    renderer->BeginFrame();
    renderer->EndFrame();
    renderer->Present();

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    renderer->ReadFramebuffer(pixels.data(), width, height);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        EXPECT_EQ(pixels[i], 255) << "pixel " << i / 4;
        EXPECT_EQ(pixels[i + 1], 0) << "pixel " << i / 4;
        EXPECT_EQ(pixels[i + 2], 0) << "pixel " << i / 4;
    }

    window.Shutdown();
}

TEST_F(WindowlessWindowTest, ShutdownLeavesAnInvalidWindowThatCanStartAgain)
{
    Window window;
    ASSERT_TRUE(window.InitWindow(Options(RenderBackend::SOFTWARE, true))) << window.GetInitError();
    window.Shutdown();

    EXPECT_FALSE(window.IsValid());
    EXPECT_FALSE(window.IsWindowless());
    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(window.GetRenderer(), nullptr);
    EXPECT_EQ(window.GetInputSystem(), nullptr);
    EXPECT_EQ(window.GetWindowDimensions()[0], 0);
    EXPECT_EQ(window.GetWindowDimensions()[1], 0);
    window.Shutdown(); // twice is safe

    ASSERT_TRUE(window.InitWindow(Options(RenderBackend::SOFTWARE, true))) << window.GetInitError();
    EXPECT_TRUE(window.IsValid());
    EXPECT_TRUE(window.IsWindowless());
    window.Shutdown();
}
