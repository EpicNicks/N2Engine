#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <renderer/common/Surface.hpp>

namespace N2Engine::Input
{
    class InputDevice;
}

namespace Renderer::Common
{
    class IRenderer;
}
namespace N2Engine::Platform
{
    /**
     * What happens to a game that the platform decides (build targets stage 6; docs/platform.html). Desktop sends only
     * QuitRequested (the close button) and Resumed after a minimised window comes back; mobile sends all of them.
     */
    enum class LifecycleEvent
    {
        /// The game went to the background: stop the clock, pause audio, save if it matters
        Paused,
        /// Back in the foreground
        Resumed,
        /// The surface went away (Android destroys its window in the background): the renderer must let go of it
        SurfaceLost,
        /// A new surface exists (NativeSurface() is the new one): the renderer takes it
        SurfaceRecreated,
        /// The system is short of memory: drop caches
        LowMemory,
        /// The user or the system asked to quit (the close button, the last activity finishing)
        QuitRequested,
        /// The back button or gesture (Android); a game that ignores it gets QuitRequested from the system later
        BackRequested,
    };

    [[nodiscard]] std::string_view LifecycleEventName(LifecycleEvent event);

    /// The part of the surface the system draws over (a notch, rounded corners, the home indicator), in pixels
    struct SafeAreaInsets
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;

        bool operator==(const SafeAreaInsets &) const = default;
    };

    /// What a renderer draws on: the platform's own handle, by kind (stage 7 hands it to the renderers)
    struct NativeSurface
    {
        enum class Kind
        {
            /// No surface: the renderer draws offscreen (headless, the web's software renderer)
            None,
            /// A GLFW window (desktop-glfw): handle is the GLFWwindow*
            GlfwWindow,
            /// An Android ANativeWindow*
            AndroidWindow,
            /// A CAMetalLayer* (iOS, macOS)
            MetalLayer,
            /// A canvas element's selector (the web): handle is a const char*
            WebCanvas,
            /// A private target's own kind (bt11): its backend and renderer agree on what handle is
            Custom,
        };
        Kind kind = Kind::None;
        void *handle = nullptr;
        /// A second handle when the kind needs one (an X11 Display*, say); null otherwise
        void *display = nullptr;
        /// Size in pixels
        int width = 0;
        int height = 0;
    };

    /// The folders a game uses (docs/packs.html for the read-only side)
    struct HostPaths
    {
        /// Where the game's project (or its pack's mount point) is
        std::filesystem::path projectRoot;
        /// user://, writable, the game's own
        std::filesystem::path userData;
        /// A writable folder the system may clear
        std::filesystem::path temp;
    };

    /// How the host is asked to make its surface
    struct SurfaceOptions
    {
        std::string title = "N2Engine";
        /// The window's size; 0: half the primary monitor (desktop), else FallbackWidth x FallbackHeight
        int width = 0;
        int height = 0;
        /// An OpenGL 3.3 core context on the window (desktop-glfw: the OpenGL and software renderers present through
        /// it); none for Vulkan, Metal and every offscreen renderer
        bool openGlContext = true;
        /// An sRGB-capable framebuffer (the OpenGL renderer's linear lighting)
        bool srgb = false;
        /// Shown on screen; a hidden window still renders (CI, tests with a real context)
        bool visible = true;

        static constexpr int FallbackWidth = 1280;
        static constexpr int FallbackHeight = 720;
    };

    enum class WindowMode
    {
        Windowed,
        Fullscreen,
        /// A window without decorations covering its monitor
        BorderlessWindowed,
    };

    /**
     * Everything the engine asks of the platform, one backend per platform: desktop-glfw (Windows, Linux, macOS), headless
     * (no window: the editor host, n2server, the tests, the replay legs), web, and later android, ios and private targets.
     * GLFW is used only inside desktop-glfw.
     *
     * The loop: a host that OwnsLoop() (web, Android, iOS) calls Application::RunFrame itself, from the OS's frame
     * callback; otherwise the engine runs its own loop (Application::Run) and calls PollEvents once a frame.
     */
    class IPlatformHost
    {
    public:
        virtual ~IPlatformHost() = default;

        /// "desktop-glfw", "headless", "web", ...
        [[nodiscard]] virtual std::string_view Name() const = 0;

        // ---- the surface
        /// Makes the window or surface; false with the reason in LastError(). A host without one (headless) succeeds and
        /// reports NativeSurface::Kind::None.
        virtual bool CreateSurface(const SurfaceOptions &options) = 0;
        virtual void DestroySurface() = 0;
        [[nodiscard]] virtual NativeSurface Surface() const = 0;
        /// The surface as a renderer takes it (build targets stage 7): the native handle and the window system's
        /// operations the renderers need (an OpenGL context, a Vulkan surface, the size in pixels). None: draw offscreen.
        [[nodiscard]] virtual Renderer::Common::Surface RenderSurface() const { return {}; }
        [[nodiscard]] virtual std::string LastError() const = 0;
        /// The surface's size in pixels (the framebuffer), and the scale from window units to pixels
        [[nodiscard]] virtual int SurfaceWidth() const = 0;
        [[nodiscard]] virtual int SurfaceHeight() const = 0;
        [[nodiscard]] virtual float ContentScale() const { return 1.0f; }
        [[nodiscard]] virtual SafeAreaInsets Insets() const { return {}; }
        /// Called with the new size in pixels whenever the surface's size changes
        virtual void SetResizeHandler(std::function<void(int, int)> handler) = 0;

        // ---- the window (no-ops where there is none)
        virtual void SetTitle(const std::string &title) = 0;
        virtual void SetWindowMode(WindowMode mode) = 0;
        [[nodiscard]] virtual WindowMode GetWindowMode() const = 0;
        virtual void SetCursorVisible(bool visible) { (void)visible; }
        [[nodiscard]] virtual std::optional<std::string> ClipboardText() const { return std::nullopt; }
        virtual void SetClipboardText(const std::string &text) { (void)text; }
        /// The soft keyboard on mobile (text fields); nothing on desktop
        virtual void SetTextInputActive(bool active) { (void)active; }
        virtual void SetKeepScreenOn(bool on) { (void)on; }

        // ---- events, lifecycle and the loop
        /// Delivers the platform's pending events (input, resize, close). Once a frame, before the input system updates.
        virtual void PollEvents() = 0;
        /// Whether the user or the system asked to quit
        [[nodiscard]] virtual bool ShouldClose() const = 0;
        /// Called with every lifecycle event, as it happens
        virtual void SetLifecycleHandler(std::function<void(LifecycleEvent)> handler) = 0;
        /// Whether the OS owns the frame loop (the engine is called once a frame) rather than the engine
        [[nodiscard]] virtual bool OwnsLoop() const = 0;

        // ---- input and paths
        /// The device the input system reads (engine codes; InputDevice.hpp), or null when there is none
        [[nodiscard]] virtual Input::InputDevice *Input() = 0;
        [[nodiscard]] virtual HostPaths Paths() const = 0;

        // ---- a platform's own renderer (build targets stage 11; docs/private-targets.html)
        /// A renderer this platform brings itself (a private target's GPU API), used instead of the configured backend
        /// (Config::ApplicationOptions::renderBackend) and initialised with RenderSurface() like any other. Null, the
        /// default: the engine's own renderers. (Defined out of line, in HeadlessHost.cpp, where IRenderer is complete.)
        [[nodiscard]] virtual std::unique_ptr<Renderer::Common::IRenderer> CreateRenderer();
    };

    /// The backends this build has. A host made for a platform that is not this one is null.
    [[nodiscard]] std::unique_ptr<IPlatformHost> MakeHeadlessHost(HostPaths paths = {});
    /// GLFW (Windows, Linux, macOS); null in a build without GLFW (the web)
    [[nodiscard]] std::unique_ptr<IPlatformHost> MakeDesktopGlfwHost(HostPaths paths = {});

    /**
     * Hands a lifecycle event to a headless host, which delivers it to its handler at once (a headless program has no OS
     * to send them: tests, and a host that relays them from elsewhere, such as a browser tab's visibility). False for any
     * other backend, whose events come from its platform.
     */
    bool PostLifecycleEvent(IPlatformHost &host, LifecycleEvent event);
}
