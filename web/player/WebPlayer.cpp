// The N2Engine web player (stage 2 of issue #155; docs/web-build.html): runs a project folder in a browser tab with the
// software renderer, single-threaded, and shows each frame on a 2D canvas. Built only under N2ENGINE_PLATFORM_WEB.
//
//  - The engine runs windowless (isHeadless + software renderer: no GLFW at all), so there is no GL context and no
//    native window. Each frame is read back from the renderer and handed to the page's JavaScript (Module.n2Present),
//    which puts it on the canvas. The page, not this file, owns the canvas; under node (the CI smoke) a mock stands in.
//  - The browser calls Frame() through emscripten_set_main_loop: a blocking loop would freeze the tab. Frame() is one
//    Application::RunFrame, the body of Application::Run, so the fixed-step accumulator behaves as on every platform.
//  - Keyboard and mouse come from HTML5 events: keys and buttons go to an Input::InjectedKeys installed as the engine's
//    KeySource (the seam the editor's play child uses), the pointer to Mouse::InjectPointer, the wheel to
//    Mouse::AccumulateScroll. WebInputMap.hpp holds the translation.
//
// Arguments (Module.arguments, or callMain's): [--project DIR] (default /project, the preloaded folder),
// [--frames N] (run N frames, then stop: the node smoke test, which also skips the DOM and takes its size from --size),
// [--size WxH] (the render size when there is no canvas to measure).
#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <renderer/common/FrameRows.hpp>

#include "WebInputMap.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/KeySource.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;

// The page's side of the player. Each reads a hook off the Module object (see web/shell.html) and does nothing when the
// page has not set it, so the same binary runs under node.
EM_JS(void, n2_present, (const std::uint8_t *pixels, int width, int height), {
    const hook = Module['n2Present'];
    if (hook) hook(HEAPU8.subarray(pixels, pixels + width * height * 4), width, height);
});

EM_JS(void, n2_notify_stopped, (int frames), {
    const hook = Module['n2Stopped'];
    if (hook) hook(frames);
});

EM_JS(void, n2_notify_failed, (const char *message), {
    const hook = Module['n2Failed'];
    if (hook) hook(UTF8ToString(message));
});

// The canvas's box in viewport CSS pixels: left, top, width, height. 0 when there is no canvas element.
EM_JS(int, n2_canvas_rect, (double *out), {
    const canvas = typeof document !== 'undefined' ? document.getElementById('canvas') : null;
    if (!canvas) return 0;
    const box = canvas.getBoundingClientRect();
    HEAPF64[out >> 3] = box.left;
    HEAPF64[(out >> 3) + 1] = box.top;
    HEAPF64[(out >> 3) + 2] = box.width;
    HEAPF64[(out >> 3) + 3] = box.height;
    return 1;
});

namespace
{
    /// The most pixels wide a frame renders. The canvas is shown at its CSS size whatever this is (the browser scales it).
    constexpr int MaxRenderWidth = 1280;

    struct Player
    {
        Input::InjectedKeys keys;
        std::uint32_t buttons = 0;
        std::uint32_t pressedSinceSample = 0; // presses latched so a click shorter than a frame still reaches the game
        Math::Vector2 pointer{0.0f, 0.0f};
        bool domAvailable = true;  // false under node (--frames): no canvas to measure, no events to listen to
        int smokeFrames = 0;       // > 0: stop after this many frames
        int frames = 0;
        Web::RenderSize size{};    // the size frames render at now
        std::vector<std::uint8_t> frame; // the last frame read back, top row first, RGBA8
        bool ended = false;
    };

    Player g_player;

    // -- input ---------------------------------------------------------------------------------------------------

    EM_BOOL OnKey(const int type, const EmscriptenKeyboardEvent *event, void *)
    {
        const Input::Key key = Web::KeyFromDomCode(event->code);
        if (key == Input::Key::Unknown)
        {
            return EM_FALSE;
        }
        g_player.keys.SetKey(key, type == EMSCRIPTEN_EVENT_KEYDOWN);
        // A key with a browser meaning (scrolling, tabbing) is the game's while it runs, unless a shortcut modifier is held
        const bool shortcut = event->ctrlKey || event->altKey || event->metaKey;
        return Web::SuppressesBrowserDefault(key) && !shortcut ? EM_TRUE : EM_FALSE;
    }

    void ReleaseAllInput()
    {
        g_player.keys.ReleaseAll();
        g_player.buttons = 0;
        g_player.pressedSinceSample = 0;
    }

    EM_BOOL OnBlur(int, const EmscriptenFocusEvent *, void *)
    {
        // A key released while the page was not focused never reports: let go of everything rather than stick
        ReleaseAllInput();
        return EM_FALSE;
    }

    bool CanvasRect(double (&rect)[4])
    {
        return n2_canvas_rect(rect) != 0 && rect[2] > 0.0 && rect[3] > 0.0;
    }

    void UpdatePointer(const EmscriptenMouseEvent *event)
    {
        double rect[4];
        if (!CanvasRect(rect))
        {
            return;
        }
        g_player.pointer = Math::Vector2(
            Web::ClientToRender(static_cast<double>(event->clientX), rect[0], rect[2], g_player.size.width),
            Web::ClientToRender(static_cast<double>(event->clientY), rect[1], rect[3], g_player.size.height));
    }

    EM_BOOL OnMouse(const int type, const EmscriptenMouseEvent *event, void *)
    {
        UpdatePointer(event);
        if (type == EMSCRIPTEN_EVENT_MOUSEMOVE)
        {
            return EM_FALSE;
        }
        const auto button = Web::MouseButtonFromDom(event->button);
        if (!button)
        {
            return EM_FALSE;
        }
        const bool down = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
        if (down)
        {
            // Only a press that starts on the canvas is the game's; the release is taken from anywhere, so a button
            // held while the pointer leaves the canvas is not stuck down
            double rect[4];
            if (!CanvasRect(rect) || event->clientX < rect[0] || event->clientX >= rect[0] + rect[2] ||
                event->clientY < rect[1] || event->clientY >= rect[1] + rect[3])
            {
                return EM_FALSE;
            }
        }
        g_player.keys.SetMouseButton(*button, down);
        const std::uint32_t bit = Input::Mouse::ButtonBit(static_cast<int>(*button));
        g_player.buttons = down ? (g_player.buttons | bit) : (g_player.buttons & ~bit);
        if (down)
        {
            g_player.pressedSinceSample |= bit;
        }
        return down ? EM_TRUE : EM_FALSE;
    }

    EM_BOOL OnWheel(int, const EmscriptenWheelEvent *event, void *)
    {
        if (auto *mouse = Input::Mouse::Get())
        {
            mouse->AccumulateScroll(Web::WheelToScroll(event->deltaX, static_cast<int>(event->deltaMode), false),
                                    Web::WheelToScroll(event->deltaY, static_cast<int>(event->deltaMode), true));
        }
        return EM_TRUE; // the page does not scroll under the game
    }

    void RegisterInput()
    {
        const char *const document = EMSCRIPTEN_EVENT_TARGET_DOCUMENT;
        const char *const window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
        emscripten_set_keydown_callback(document, nullptr, EM_TRUE, OnKey);
        emscripten_set_keyup_callback(document, nullptr, EM_TRUE, OnKey);
        emscripten_set_blur_callback(window, nullptr, EM_TRUE, OnBlur);
        // The window, not the canvas, for the pointer: a drag that leaves the canvas keeps reporting, and the release
        // outside it still arrives. clientX/Y are viewport coordinates, which UpdatePointer maps onto the canvas.
        emscripten_set_mousemove_callback(window, nullptr, EM_TRUE, OnMouse);
        emscripten_set_mousedown_callback(window, nullptr, EM_TRUE, OnMouse);
        emscripten_set_mouseup_callback(window, nullptr, EM_TRUE, OnMouse);
        emscripten_set_wheel_callback("#canvas", nullptr, EM_TRUE, OnWheel);
    }

    // -- size and frames -----------------------------------------------------------------------------------------

    /// Makes the render size follow the canvas as shown. False when there is nothing to render into.
    bool FollowCanvasSize()
    {
        if (!g_player.domAvailable)
        {
            return true; // the node smoke: the size from --size stays
        }
        double rect[4];
        if (!CanvasRect(rect))
        {
            return false;
        }
        const Web::RenderSize wanted = Web::FitRenderSize(rect[2], rect[3], MaxRenderWidth);
        if (wanted.width == 0 || wanted == g_player.size)
        {
            return true;
        }
        if (Application::GetInstance().GetWindow().SetRenderSize(wanted.width, wanted.height))
        {
            g_player.size = wanted;
            emscripten_set_canvas_element_size("#canvas", wanted.width, wanted.height);
        }
        return true;
    }

    void PresentFrame()
    {
        auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
        if (!renderer || g_player.size.width <= 0 || g_player.size.height <= 0)
        {
            return;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(g_player.size.width) * 4;
        g_player.frame.resize(rowBytes * static_cast<std::size_t>(g_player.size.height));
        renderer->ReadFramebuffer(g_player.frame.data(), g_player.size.width, g_player.size.height);
        // ReadFramebuffer gives the bottom row first (as glReadPixels does); a canvas wants the top row first
        Renderer::Common::FlipRows(g_player.frame.data(), rowBytes, static_cast<std::size_t>(g_player.size.height));
        // The software renderer stores its alpha as written; the canvas premultiplies it, so show every pixel opaque
        for (std::size_t i = 3; i < g_player.frame.size(); i += 4)
        {
            g_player.frame[i] = 255;
        }
        n2_present(g_player.frame.data(), g_player.size.width, g_player.size.height);
    }

    void Stop()
    {
        if (g_player.ended)
        {
            return;
        }
        g_player.ended = true;
        emscripten_cancel_main_loop();
        Application::GetInstance().EndRun();
        if (g_player.smokeFrames > 0)
        {
            std::printf("N2_WEB_SMOKE_DONE frames=%d size=%dx%d\n", g_player.frames, g_player.size.width,
                        g_player.size.height);
        }
        n2_notify_stopped(g_player.frames);
    }

    /// One browser frame (emscripten_set_main_loop): the canvas size, the pointer, one engine frame, then the picture
    void Frame()
    {
        Application &application = Application::GetInstance();
        if (!FollowCanvasSize())
        {
            return; // no canvas yet or hidden: skip the frame, the loop goes on
        }
        if (auto *mouse = Input::Mouse::Get())
        {
            mouse->InjectPointer(g_player.pointer, g_player.buttons | g_player.pressedSinceSample);
            g_player.pressedSinceSample = 0;
        }
        if (!application.RunFrame())
        {
            Stop(); // Quit() or a closed window: the game ended itself
            return;
        }
        ++g_player.frames;
        PresentFrame();
        if (g_player.smokeFrames > 0 && g_player.frames >= g_player.smokeFrames)
        {
            Stop();
        }
    }

    // -- startup -------------------------------------------------------------------------------------------------

    bool ParseSize(const std::string_view text, Web::RenderSize &out)
    {
        const auto x = text.find('x');
        if (x == std::string_view::npos)
        {
            return false;
        }
        const int width = std::atoi(std::string(text.substr(0, x)).c_str());
        const int height = std::atoi(std::string(text.substr(x + 1)).c_str());
        if (width <= 0 || height <= 0)
        {
            return false;
        }
        out = {width, height};
        return true;
    }

    int Fail(const std::string &message)
    {
        std::fprintf(stderr, "%s\n", message.c_str());
        n2_notify_failed(message.c_str());
        return 1;
    }
}

int main(int argc, char *argv[])
{
    std::filesystem::path projectDir = "/project";
    Web::RenderSize startSize{1280, 720};
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--project" && hasValue)
        {
            projectDir = argv[++i];
        }
        else if (arg == "--frames" && hasValue)
        {
            g_player.smokeFrames = std::max(0, std::atoi(argv[++i]));
            g_player.domAvailable = g_player.smokeFrames == 0;
        }
        else if (arg == "--size" && hasValue)
        {
            if (!ParseSize(argv[++i], startSize))
            {
                return Fail("--size needs WIDTHxHEIGHT");
            }
        }
        else
        {
            return Fail(std::format("Unknown option '{}'. Usage: [--project DIR] [--frames N] [--size WxH]", arg));
        }
    }

    Application &application = Application::GetInstance();
    // Headless is the engine's windowless software path (Window::UsesNoWindow): no GLFW, frames read back and shown by
    // the page. On the web it does not send the sound to a loopback device: Application opens the browser's output
    // (Emscripten's OpenAL on WebAudio), so the game's sounds play from the page. That context is created here, inside
    // the Start button's click (callMain), and the loader resumes it on the first gesture if the browser kept it
    // suspended (web/n2-loader.js). With no WebAudio (node, a browser without it) audio stays off with one warning.
    application.Init({
        .projectPath = projectDir.string(),
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
        .renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE,
        .isHeadless = true,
    });
    if (!application.GetWindow().IsValid())
    {
        return Fail("The engine could not start (no renderer): " + application.GetWindow().GetInitError());
    }
    // The size frames render at, from here on: the canvas's (FollowCanvasSize), or --size without one
    if (!application.GetWindow().SetRenderSize(startSize.width, startSize.height))
    {
        return Fail("The renderer could not be sized");
    }
    g_player.size = startSize;
    if (g_player.domAvailable)
    {
        emscripten_set_canvas_element_size("#canvas", startSize.width, startSize.height);
    }

    SceneManager::AddScene(Scene::Create("Web Scene"), true);
    SceneManager::ProcessAnyPendingSceneChange();

    // As lua_project/src/Main.cpp: scripts load through ResourceLoader, which resolves res:// under <project>/assets
    if (const auto project = IO::ProjectFile::Load(projectDir); project)
    {
        IO::ResourceUUID::Initialize(project->projectId);
        IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath());
        for (const std::string &problem : ApplyProjectSettings(project->settings))
        {
            Logger::Warn("Project settings: " + problem);
        }
    }
    else
    {
        Logger::Warn(project.error() + "; running the folder without one");
        IO::ResourceUUID::Initialize(IO::ResourceUUID::NamespaceForProjectDir(projectDir));
        IO::ResourceLoader::Instance().Initialize(projectDir);
    }

    if (!Scripting::LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua")))
    {
        return Fail("The project's scene.lua failed to run (see the log)");
    }

    // Keys and mouse buttons come from the page's events, not from a window
    Input::KeySource::Set(&g_player.keys);
    if (g_player.domAvailable)
    {
        RegisterInput();
    }

    if (!application.BeginRun())
    {
        return Fail("The engine could not start running");
    }
    // fps 0: the browser's own pacing (requestAnimationFrame, or a timer where there is none). simulate_infinite_loop:
    // main does not return while the loop runs, so the objects above stay alive.
    emscripten_set_main_loop(Frame, 0, EM_TRUE);
    return 0;
}
