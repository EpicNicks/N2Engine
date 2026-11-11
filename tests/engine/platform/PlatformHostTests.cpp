#include <gtest/gtest.h>

#include <vector>

#include <engine/Window.hpp>
#include <engine/config/ApplicationOptions.hpp>
#include <engine/platform/PlatformHost.hpp>

// The platform layer (build targets stage 6; docs/platform.html): the headless backend, lifecycle events reaching the
// engine, and a fake backend written against the interface alone (what an Android or iOS backend implements).
using namespace N2Engine;
using namespace N2Engine::Platform;

namespace
{
    /// Every member a backend implements, with mobile behaviour: the OS owns the loop, the surface comes and goes
    class FakeMobileHost final : public IPlatformHost
    {
    public:
        std::function<void(LifecycleEvent)> lifecycle;
        std::function<void(int, int)> resize;
        bool surface = false;
        bool keepScreenOn = false;
        bool textInput = false;

        std::string_view Name() const override { return "fake-mobile"; }
        bool CreateSurface(const SurfaceOptions &) override { return surface = true; }
        void DestroySurface() override { surface = false; }
        NativeSurface Surface() const override
        {
            return surface ? NativeSurface{NativeSurface::Kind::AndroidWindow, reinterpret_cast<void *>(0x1), nullptr, 1080, 2400}
                           : NativeSurface{};
        }
        std::string LastError() const override { return {}; }
        int SurfaceWidth() const override { return surface ? 1080 : 0; }
        int SurfaceHeight() const override { return surface ? 2400 : 0; }
        float ContentScale() const override { return 2.75f; }
        SafeAreaInsets Insets() const override { return {0, 96, 0, 48}; }
        void SetResizeHandler(std::function<void(int, int)> handler) override { resize = std::move(handler); }
        void SetTitle(const std::string &) override {}
        void SetWindowMode(WindowMode) override {}
        WindowMode GetWindowMode() const override { return WindowMode::Fullscreen; }
        void SetTextInputActive(const bool active) override { textInput = active; }
        void SetKeepScreenOn(const bool on) override { keepScreenOn = on; }
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        void SetLifecycleHandler(std::function<void(LifecycleEvent)> handler) override { lifecycle = std::move(handler); }
        bool OwnsLoop() const override { return true; }
        Input::InputDevice *Input() override { return nullptr; }
        HostPaths Paths() const override { return {"/data/app/game", "/data/user/0/game/files", "/data/user/0/game/cache"}; }

        /// What the OS does when the app goes to the background and back
        void Background()
        {
            lifecycle(LifecycleEvent::Paused);
            DestroySurface();
            lifecycle(LifecycleEvent::SurfaceLost);
        }
        void Foreground()
        {
            CreateSurface({});
            lifecycle(LifecycleEvent::SurfaceRecreated);
            lifecycle(LifecycleEvent::Resumed);
        }
    };
}

TEST(PlatformHostTest, TheHeadlessBackendHasNoWindowNoInputAndNeverCloses)
{
    auto host = MakeHeadlessHost({"/game", "/user", "/tmp"});
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->Name(), "headless");
    EXPECT_TRUE(host->CreateSurface({}));
    EXPECT_EQ(host->Surface().kind, NativeSurface::Kind::None);
    EXPECT_EQ(host->SurfaceWidth(), SurfaceOptions::FallbackWidth);
    EXPECT_EQ(host->SurfaceHeight(), SurfaceOptions::FallbackHeight);
    EXPECT_EQ(host->Input(), nullptr) << "input is injected without a window";
    EXPECT_FALSE(host->ShouldClose());
    EXPECT_FALSE(host->OwnsLoop());
    EXPECT_EQ(host->Paths().userData, std::filesystem::path("/user"));
    EXPECT_EQ(host->Insets(), SafeAreaInsets{});
    host->PollEvents(); // nothing to deliver
    host->DestroySurface();
    EXPECT_EQ(host->SurfaceWidth(), 0);
}

TEST(PlatformHostTest, EveryLifecycleEventHasAName)
{
    for (const auto event : {LifecycleEvent::Paused, LifecycleEvent::Resumed, LifecycleEvent::SurfaceLost,
                             LifecycleEvent::SurfaceRecreated, LifecycleEvent::LowMemory, LifecycleEvent::QuitRequested,
                             LifecycleEvent::BackRequested})
    {
        EXPECT_NE(LifecycleEventName(event), "Unknown");
    }
}

TEST(PlatformHostTest, AWindowlessWindowRunsOnTheHeadlessBackendAndForwardsLifecycleEvents)
{
    Window window;
    std::vector<LifecycleEvent> seen;
    window.SetLifecycleHandler([&](const LifecycleEvent event) { seen.push_back(event); });
    Config::ApplicationOptions options;
    options.isHeadless = true;
    options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
    ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
    ASSERT_NE(window.GetPlatformHost(), nullptr);
    EXPECT_EQ(window.GetPlatformHost()->Name(), "headless");
    EXPECT_EQ(window.GetInputDevice(), nullptr);
    EXPECT_FALSE(Window::HasGlfw()) << "no GLFW without a window";

    EXPECT_TRUE(PostLifecycleEvent(*window.GetPlatformHost(), LifecycleEvent::Paused));
    EXPECT_TRUE(PostLifecycleEvent(*window.GetPlatformHost(), LifecycleEvent::Resumed));
    EXPECT_EQ(seen, (std::vector<LifecycleEvent>{LifecycleEvent::Paused, LifecycleEvent::Resumed}));

    window.Shutdown();
    EXPECT_EQ(window.GetPlatformHost(), nullptr);
}

TEST(PlatformHostTest, TheInterfaceCoversAMobileBackend)
{
    FakeMobileHost host;
    std::vector<LifecycleEvent> seen;
    host.SetLifecycleHandler([&](const LifecycleEvent event) { seen.push_back(event); });
    ASSERT_TRUE(host.CreateSurface({}));
    EXPECT_TRUE(host.OwnsLoop()) << "the OS calls the engine once a frame";
    EXPECT_EQ(host.Surface().kind, NativeSurface::Kind::AndroidWindow);
    EXPECT_EQ(host.Insets().top, 96) << "the notch";
    host.SetKeepScreenOn(true);
    host.SetTextInputActive(true);
    EXPECT_TRUE(host.keepScreenOn && host.textInput);

    host.Background();
    EXPECT_EQ(host.Surface().kind, NativeSurface::Kind::None) << "the surface is gone in the background";
    host.Foreground();
    EXPECT_EQ(seen, (std::vector<LifecycleEvent>{LifecycleEvent::Paused, LifecycleEvent::SurfaceLost,
                                                LifecycleEvent::SurfaceRecreated, LifecycleEvent::Resumed}));
    EXPECT_FALSE(PostLifecycleEvent(host, LifecycleEvent::LowMemory)) << "only the headless backend takes posted events";
}
