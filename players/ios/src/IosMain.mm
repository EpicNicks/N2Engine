// The iOS player (build targets stage 10; docs/apple.html): a UIKit app with one view. The engine starts when the view is
// on screen, then a CADisplayLink runs one frame per screen refresh while the app is active (the OS owns the loop:
// Application::BeginRun, RunFrame, EndRun). The software renderer's frame becomes the view's layer contents.
//
// The game is one pack, game.n2pack, in the app bundle. Bundle files are ordinary files, so the pack is mounted where it is
// (no copy, unlike Android's APK assets).
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <engine/Application.hpp>
#include <engine/Logger.hpp>
#include <engine/ProjectSettings.hpp>
#include <engine/input/Touchscreen.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/io/Vfs.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneFile.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/scripting/LuaRuntime.hpp>

#include "IosHost.hpp"

using namespace N2Engine;

namespace
{
    struct PlayerState
    {
        Ios::IosHost *host = nullptr; // owned by the Window once the engine started
        bool engineStarted = false;
        bool engineFailed = false;
        bool ended = false;
        std::uint64_t frames = 0;
        std::uint64_t framesAtLastReport = 0;
        std::chrono::steady_clock::time_point lastReport{};
    };
    PlayerState g_state;

    /// The log on stdout too: `xcrun simctl launch --console` and Xcode show it (the simulator smoke test reads it)
    void LogToConsole(const std::string_view message, const Logger::LogLevel level)
    {
        const char *prefix = level == Logger::LogLevel::Error ? "error" : level == Logger::LogLevel::Warn ? "warning" : "info";
        std::fprintf(stdout, "N2Engine %s: %.*s\n", prefix, static_cast<int>(message.size()), message.data());
        std::fflush(stdout);
    }

    /// "N2Engine iOS player: <frames> frames, <fps> fps" every five seconds of running, as the Android player logs
    void CountFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_state.frames++ == 0)
        {
            g_state.lastReport = now;
            Logger::Info("N2Engine iOS player: first frame");
            return;
        }
        const std::chrono::duration<double> elapsed = now - g_state.lastReport;
        if (elapsed.count() >= 5.0)
        {
            const double fps = static_cast<double>(g_state.frames - g_state.framesAtLastReport) / elapsed.count();
            Logger::Info(std::format("N2Engine iOS player: {} frames, {:.1f} fps", g_state.frames, fps));
            g_state.framesAtLastReport = g_state.frames;
            g_state.lastReport = now;
        }
    }

    /// The project's first scene: its startupScene when it names one, else what assets/scene.lua builds (as the other players)
    bool StartGame(const IO::ProjectFile &project)
    {
        if (!project.startupScene.empty())
        {
            const auto file = IO::ResourceLoader::Instance().Load<SceneFile>(IO::ResourcePath(project.startupScene));
            std::unique_ptr<Scene> scene = file ? Scene::FromJSON(file->GetData(), true) : nullptr;
            if (scene == nullptr)
            {
                Logger::Error(std::format("The startup scene {} could not be read (see the log)", project.startupScene));
                return false;
            }
            scene->SetUUID(file->GetUUID());
            SceneManager::AddScene(std::move(scene), true);
            SceneManager::ProcessAnyPendingSceneChange();
            return true;
        }
        SceneManager::AddScene(Scene::Create(project.name.empty() ? std::string("Game") : project.name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return Scripting::LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua"));
    }

    std::filesystem::path PathOf(NSString *path)
    {
        return std::filesystem::path(std::string(path.UTF8String));
    }
}

// ---- the view: shows frames, reports touches ------------------------------------------------------------------------

@interface N2View : UIView
@end

@implementation N2View
{
    std::map<UITouch *, int> _touchIds;
    int _nextTouchId;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.blackColor;
        self.layer.contentsGravity = kCAGravityResize;
        // The frame is the view's size in pixels: no smoothing when the layer shows it
        self.layer.magnificationFilter = kCAFilterNearest;
    }
    return self;
}

- (Math::Vector2)pixelsOf:(UITouch *)touch
{
    const CGPoint point = [touch locationInView:self];
    const CGFloat scale = self.contentScaleFactor;
    return Math::Vector2(static_cast<float>(point.x * scale), static_cast<float>(point.y * scale));
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    for (UITouch *touch in touches)
    {
        const int id = _nextTouchId++;
        _touchIds[touch] = id;
        const Math::Vector2 position = [self pixelsOf:touch];
        Input::Touchscreen::Get().Down(id, position);
        if (g_state.host != nullptr && _touchIds.size() == 1)
        {
            g_state.host->InputDevice().OnPointer(position.x, position.y, true);
        }
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    for (UITouch *touch in touches)
    {
        const auto found = _touchIds.find(touch);
        if (found == _touchIds.end()) continue;
        const Math::Vector2 position = [self pixelsOf:touch];
        Input::Touchscreen::Get().Down(found->second, position); // Down on a held finger moves it
        if (g_state.host != nullptr && found == _touchIds.begin())
        {
            g_state.host->InputDevice().OnPointer(position.x, position.y, true);
        }
    }
}

- (void)endTouches:(NSSet<UITouch *> *)touches cancelled:(BOOL)cancelled
{
    for (UITouch *touch in touches)
    {
        const auto found = _touchIds.find(touch);
        if (found == _touchIds.end()) continue;
        const Math::Vector2 position = [self pixelsOf:touch];
        if (cancelled) Input::Touchscreen::Get().Cancel(found->second);
        else Input::Touchscreen::Get().Up(found->second, position);
        _touchIds.erase(found);
        if (g_state.host != nullptr && _touchIds.empty())
        {
            g_state.host->InputDevice().OnPointer(position.x, position.y, false);
        }
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self endTouches:touches cancelled:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self endTouches:touches cancelled:YES];
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    if (g_state.host != nullptr) g_state.host->OnResized();
}

/// A frame from the software renderer (RGBA8, rows bottom to top) as the layer's contents
- (void)showFrame:(const std::uint8_t *)rgba width:(int)width height:(int)height
{
    if (width <= 0 || height <= 0) return;
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    NSMutableData *flipped = [NSMutableData dataWithLength:row * static_cast<std::size_t>(height)];
    auto *out = static_cast<std::uint8_t *>(flipped.mutableBytes);
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(out + static_cast<std::size_t>(y) * row, rgba + static_cast<std::size_t>(height - 1 - y) * row, row);
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)flipped);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(static_cast<size_t>(width), static_cast<size_t>(height), 8, 32, row, space,
                                     kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    if (image != nullptr)
    {
        [CATransaction begin];
        [CATransaction setDisableActions:YES]; // no fade between frames
        self.layer.contents = (__bridge id)image;
        [CATransaction commit];
        CGImageRelease(image);
    }
}
@end

// ---- the view controller: starts the engine, runs the frames --------------------------------------------------------

@interface N2ViewController : UIViewController
@end

@implementation N2ViewController
{
    CADisplayLink *_displayLink;
}

- (void)loadView
{
    self.view = [[N2View alloc] initWithFrame:UIScreen.mainScreen.bounds];
}

- (BOOL)prefersStatusBarHidden
{
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
    return YES;
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    if (!g_state.engineStarted && !g_state.engineFailed)
    {
        g_state.engineStarted = [self startEngine];
        g_state.engineFailed = !g_state.engineStarted;
        if (g_state.engineStarted)
        {
            _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
            [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        }
    }
}

- (BOOL)startEngine
{
    NSString *packPath = [NSBundle.mainBundle pathForResource:@"game" ofType:@"n2pack"];
    if (packPath == nil)
    {
        Logger::Error("The app has no game.n2pack (n2build puts the game's pack in the bundle)");
        return NO;
    }
    auto mount = IO::Pack::Open(PathOf(packPath));
    if (!mount)
    {
        Logger::Error("The game's pack can't be read: " + mount.error());
        return NO;
    }
    // The mount point: a folder that need not exist, where the game's folder would be
    const std::filesystem::path projectDir = PathOf(NSBundle.mainBundle.resourcePath) / "game";
    IO::Vfs::Instance().Mount(projectDir, std::move(*mount));
    const auto project = IO::ProjectFile::Load(projectDir);
    if (!project)
    {
        Logger::Error(project.error());
        return NO;
    }

    NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    const std::filesystem::path userData = PathOf(support.path) / "user";
    std::error_code ignored;
    std::filesystem::create_directories(userData, ignored);
    Platform::HostPaths paths{projectDir, userData, PathOf(NSTemporaryDirectory())};

    N2View *view = (N2View *)self.view;
    __weak N2View *weakView = view;
    Ios::ViewBridge bridge;
    bridge.layer = (__bridge void *)view.layer;
    bridge.sizeInPixels = [weakView] {
        N2View *v = weakView;
        if (v == nil) return std::pair<int, int>{0, 0};
        const CGFloat scale = v.contentScaleFactor;
        return std::pair<int, int>{static_cast<int>(v.bounds.size.width * scale), static_cast<int>(v.bounds.size.height * scale)};
    };
    bridge.scale = [weakView] { N2View *v = weakView; return v == nil ? 1.0f : static_cast<float>(v.contentScaleFactor); };
    bridge.insets = [weakView] {
        N2View *v = weakView;
        if (v == nil) return Platform::SafeAreaInsets{};
        const UIEdgeInsets i = v.safeAreaInsets;
        const CGFloat s = v.contentScaleFactor;
        return Platform::SafeAreaInsets{static_cast<int>(i.left * s), static_cast<int>(i.top * s), static_cast<int>(i.right * s),
                                        static_cast<int>(i.bottom * s)};
    };
    bridge.show = [weakView](const std::uint8_t *rgba, const int width, const int height) {
        [weakView showFrame:rgba width:width height:height];
    };
    bridge.keepScreenOn = [](const bool on) { UIApplication.sharedApplication.idleTimerDisabled = on ? YES : NO; };

    auto host = std::make_unique<Ios::IosHost>(std::move(bridge), paths);
    g_state.host = host.get();
    Application &application = Application::GetInstance();
    application.GetWindow().SetPlatformHost(std::move(host));
    application.Init({
        .projectPath = projectDir.string(),
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::AUTO,
        .renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE,
        .isHeadless = false,
    });
    if (!application.GetWindow().IsValid())
    {
        Logger::Error("No window or renderer: " + application.GetWindow().GetInitError());
        return NO;
    }
    IO::ResourceUUID::Initialize(project->projectId);
    IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath(paths.userData));
    for (const std::string &problem : ApplyProjectSettings(project->settings))
    {
        Logger::Warn("Project settings: " + problem);
    }
    return StartGame(*project) && application.BeginRun();
}

- (void)frame:(CADisplayLink *)link
{
    if (!g_state.engineStarted || g_state.ended) return;
    if (!Application::GetInstance().RunFrame())
    {
        g_state.ended = true;
        [_displayLink invalidate];
        _displayLink = nil;
        Application::GetInstance().EndRun();
        return;
    }
    CountFrame();
}

- (void)setPaused:(BOOL)paused
{
    _displayLink.paused = paused;
    if (!paused)
    {
        g_state.lastReport = std::chrono::steady_clock::now(); // the time in the background is not frame time
        g_state.framesAtLastReport = g_state.frames;
    }
}
@end

// ---- the app delegate: the lifecycle --------------------------------------------------------------------------------

@interface N2AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation N2AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    Logger::logEvent += LogToConsole;
    // Game audio: mixes with other apps' audio and follows the silent switch (AVAudioSession's ambient category)
    NSError *error = nil;
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryAmbient error:&error];
    [AVAudioSession.sharedInstance setActive:YES error:&error];

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[N2ViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (N2ViewController *)controller
{
    return (N2ViewController *)self.window.rootViewController;
}

- (void)applicationWillResignActive:(UIApplication *)application
{
    [[self controller] setPaused:YES];
    if (g_state.host != nullptr) g_state.host->OnLifecycle(Platform::LifecycleEvent::Paused);
}

- (void)applicationDidBecomeActive:(UIApplication *)application
{
    [[self controller] setPaused:NO];
    if (g_state.host != nullptr) g_state.host->OnLifecycle(Platform::LifecycleEvent::Resumed);
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application
{
    if (g_state.host != nullptr) g_state.host->OnLifecycle(Platform::LifecycleEvent::LowMemory);
}

- (void)applicationWillTerminate:(UIApplication *)application
{
    if (g_state.host != nullptr) g_state.host->OnLifecycle(Platform::LifecycleEvent::QuitRequested);
    if (g_state.engineStarted && !g_state.ended)
    {
        g_state.ended = true;
        Application::GetInstance().EndRun();
    }
}
@end

int main(int argc, char *argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([N2AppDelegate class]));
    }
}
