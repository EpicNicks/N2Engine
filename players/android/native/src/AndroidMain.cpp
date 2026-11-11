// The Android player (build targets stage 8; docs/android.html): libn2game.so, loaded by players/android's N2Activity (a
// GameActivity). android_main runs the activity's event loop; the engine starts when the first window arrives, then runs one
// frame per loop turn while the app is in the foreground (the OS owns the loop: Application::BeginRun, RunFrame, EndRun).
//
// The game is one pack, game.n2pack, in the APK's assets. It is copied once to the app's files folder (an APK asset is not
// a file the pack reader can seek in) and mounted where the game's folder would be, as on the desktop.
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <android/asset_manager.h>
#include <android/log.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <engine/Application.hpp>
#include <engine/Logger.hpp>
#include <engine/ProjectSettings.hpp>
#include <engine/input/InputSystem.hpp>
#include <engine/input/Mouse.hpp>
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

#include "AndroidHost.hpp"

namespace
{
    using namespace N2Engine;
    constexpr const char *LogTag = "N2Engine";

    struct PlayerState
    {
        Android::AndroidHost *host = nullptr; // owned by the Window once the engine started
        bool engineStarted = false;
        bool engineFailed = false;
        bool resumed = false;
        bool hasWindow = false;
        bool ended = false;
    };

    void LogToLogcat(const std::string_view message, const Logger::LogLevel level)
    {
        const int priority = level == Logger::LogLevel::Error ? ANDROID_LOG_ERROR
                             : level == Logger::LogLevel::Warn ? ANDROID_LOG_WARN
                                                               : ANDROID_LOG_INFO;
        __android_log_print(priority, LogTag, "%.*s", static_cast<int>(message.size()), message.data());
    }

    /// game.n2pack from the APK's assets, copied to dest unless a copy of the same size is there
    bool ExtractPack(AAssetManager *assets, const std::filesystem::path &dest)
    {
        AAsset *asset = AAssetManager_open(assets, "game.n2pack", AASSET_MODE_STREAMING);
        if (asset == nullptr)
        {
            Logger::Error("The APK has no assets/game.n2pack (n2build puts the game's pack there)");
            return false;
        }
        const auto size = static_cast<std::uintmax_t>(AAsset_getLength64(asset));
        std::error_code error;
        if (std::filesystem::file_size(dest, error) == size && !error)
        {
            AAsset_close(asset);
            return true; // the same pack as last time (a new APK version replaces it: its size differs, or n2build's
                         // manifest changes it; a reinstall clears the folder)
        }
        std::filesystem::create_directories(dest.parent_path(), error);
        const auto partial = std::filesystem::path(dest).concat(".part");
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            std::vector<char> buffer(1 << 16);
            int read = 0;
            while ((read = AAsset_read(asset, buffer.data(), buffer.size())) > 0)
            {
                out.write(buffer.data(), read);
            }
            if (read < 0 || !out)
            {
                AAsset_close(asset);
                Logger::Error("Couldn't copy the game's pack out of the APK");
                return false;
            }
        }
        AAsset_close(asset);
        std::filesystem::rename(partial, dest, error);
        return !error;
    }

    /// The project's first scene: its startupScene when it names one, else what assets/scene.lua builds (as players/desktop)
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

    bool StartEngine(android_app *app, PlayerState &state)
    {
        const std::filesystem::path files = app->activity->internalDataPath;
        const std::filesystem::path pack = files / "game.n2pack";
        const std::filesystem::path projectDir = files / "game";
        if (!ExtractPack(app->activity->assetManager, pack))
        {
            return false;
        }
        auto mount = IO::Pack::Open(pack);
        if (!mount)
        {
            Logger::Error("The game's pack can't be read: " + mount.error());
            return false;
        }
        IO::Vfs::Instance().Mount(projectDir, std::move(*mount));
        const auto project = IO::ProjectFile::Load(projectDir);
        if (!project)
        {
            Logger::Error(project.error());
            return false;
        }

        Platform::HostPaths paths{projectDir, files / "user", app->activity->internalDataPath};
        auto host = std::make_unique<Android::AndroidHost>(app, paths);
        state.host = host.get();
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
            return false;
        }
        IO::ResourceUUID::Initialize(project->projectId);
        IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath(paths.userData));
        for (const std::string &problem : ApplyProjectSettings(project->settings))
        {
            Logger::Warn("Project settings: " + problem);
        }
        return StartGame(*project) && application.BeginRun();
    }

    void OnAppCommand(android_app *app, const int32_t command)
    {
        auto &state = *static_cast<PlayerState *>(app->userData);
        switch (command)
        {
        case APP_CMD_INIT_WINDOW:
            state.hasWindow = true;
            if (!state.engineStarted && !state.engineFailed)
            {
                state.engineStarted = StartEngine(app, state);
                state.engineFailed = !state.engineStarted;
            }
            else if (state.host != nullptr)
            {
                state.host->OnWindowChanged();
                state.host->OnLifecycle(Platform::LifecycleEvent::SurfaceRecreated);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            state.hasWindow = false;
            if (state.host != nullptr)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::SurfaceLost);
            }
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (state.host != nullptr)
            {
                state.host->OnWindowChanged();
            }
            break;
        case APP_CMD_RESUME:
            state.resumed = true;
            if (state.host != nullptr)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::Resumed);
            }
            break;
        case APP_CMD_PAUSE:
            state.resumed = false;
            if (state.host != nullptr)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::Paused);
            }
            break;
        case APP_CMD_LOW_MEMORY:
            if (state.host != nullptr)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::LowMemory);
            }
            break;
        case APP_CMD_DESTROY:
            if (state.host != nullptr)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::QuitRequested);
            }
            break;
        default:
            break;
        }
    }

    /// Touches to the Touchscreen (the first finger is also the mouse), keys and gamepad buttons to the device
    void ProcessInput(android_app *app, PlayerState &state)
    {
        android_input_buffer *input = android_app_swap_input_buffers(app);
        if (input == nullptr || state.host == nullptr)
        {
            if (input != nullptr)
            {
                android_app_clear_motion_events(input);
                android_app_clear_key_events(input);
            }
            return;
        }
        Android::AndroidInputDevice &device = state.host->InputDevice();
        for (uint64_t i = 0; i < input->motionEventsCount; ++i)
        {
            const GameActivityMotionEvent &event = input->motionEvents[i];
            const int action = event.action & AMOTION_EVENT_ACTION_MASK;
            if (event.source & AINPUT_SOURCE_JOYSTICK)
            {
                const auto &p = event.pointers[0];
                device.OnGamepadAxes(GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_X),
                                     GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_Y),
                                     GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_Z),
                                     GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_RZ),
                                     GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_LTRIGGER),
                                     GameActivityPointerAxes_getAxisValue(&p, AMOTION_EVENT_AXIS_RTRIGGER));
                continue;
            }
            const int changed = (event.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            for (uint32_t p = 0; p < event.pointerCount; ++p)
            {
                const auto &pointer = event.pointers[p];
                const Math::Vector2 position(GameActivityPointerAxes_getX(&pointer), GameActivityPointerAxes_getY(&pointer));
                const bool isChanged = static_cast<int>(p) == changed;
                const bool up = (action == AMOTION_EVENT_ACTION_UP) ||
                                (action == AMOTION_EVENT_ACTION_POINTER_UP && isChanged);
                if (action == AMOTION_EVENT_ACTION_CANCEL)
                {
                    Input::Touchscreen::Get().Cancel(pointer.id);
                }
                else if (up)
                {
                    Input::Touchscreen::Get().Up(pointer.id, position);
                }
                else
                {
                    Input::Touchscreen::Get().Down(pointer.id, position);
                }
                if (p == 0)
                {
                    device.OnPointer(position.x, position.y, !up && action != AMOTION_EVENT_ACTION_CANCEL);
                }
            }
        }
        for (uint64_t i = 0; i < input->keyEventsCount; ++i)
        {
            const GameActivityKeyEvent &event = input->keyEvents[i];
            const bool down = event.action == AKEY_EVENT_ACTION_DOWN;
            device.OnKey(event.keyCode, down, static_cast<unsigned int>(event.unicodeChar));
            if (down && event.keyCode == AKEYCODE_BACK)
            {
                state.host->OnLifecycle(Platform::LifecycleEvent::BackRequested);
            }
        }
        android_app_clear_motion_events(input);
        android_app_clear_key_events(input);
    }
}

extern "C" void android_main(android_app *app)
{
    PlayerState state;
    app->userData = &state;
    app->onAppCmd = OnAppCommand;
    Logger::logEvent += LogToLogcat;
    // Every key goes to the game, the back button included (Escape; BackRequested)
    android_app_set_key_event_filter(app, nullptr);
    android_app_set_motion_event_filter(app, nullptr);

    Application &application = Application::GetInstance();
    while (!app->destroyRequested)
    {
        const bool running = state.engineStarted && state.resumed && state.hasWindow && !state.ended;
        // Block in the background (nothing to draw); poll and go on in the foreground
        int events = 0;
        android_poll_source *source = nullptr;
        while (ALooper_pollOnce(running ? 0 : -1, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0)
        {
            if (source != nullptr)
            {
                source->process(app, source);
            }
            if (app->destroyRequested || (!running && state.engineStarted && state.resumed && state.hasWindow))
            {
                break;
            }
        }
        ProcessInput(app, state);
        if (state.engineStarted && state.resumed && state.hasWindow && !state.ended)
        {
            if (!application.RunFrame())
            {
                state.ended = true;
                GameActivity_finish(app->activity);
            }
        }
    }
    if (state.engineStarted)
    {
        application.EndRun();
    }
}
