// Issue #445: samples/features-445, the sample project that uses the issue's features together, run headless for two
// seconds with the software renderer. Its Director script checks what it built; this test fails on any of those checks,
// on any logged error but the expected shader stand-in notice, and on a few pixels of the screen-space HUD.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "../modules/ModuleGuards.hpp"

using namespace N2Engine;
using Scripting::LuaRuntime;

#ifdef N2ENGINE_BOX2D_ENABLED

namespace
{
    constexpr int Width = 800;
    constexpr int Height = 600;

    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    class Features445SampleTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            N2_SKIP_WITHOUT_UI();
            ASSERT_TRUE(LuaRuntime::Instance().Initialize());
            IO::ResourceUUID::Initialize(Math::UUID::Random());
            IO::ResourceLoader::Instance().Initialize(N2_FEATURES_445_DIR);

            Config::ApplicationOptions options;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            ASSERT_TRUE(window.SetRenderSize(Width, Height));
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Layers::ResetToDefaults();
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(1.0 / 60.0));

            SceneManager::AddScene(Scene::Create("Features445"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _started = true;
        }

        void TearDown() override
        {
            if (!_started)
            {
                return;
            }
            SceneManager::AddScene(Scene::Create("Features445_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
        }

        /// (x, y) in window pixels, y up (ReadFramebuffer gives the bottom row first)
        static Rgb PixelAt(const std::vector<std::uint8_t> &pixels, const int x, const int y)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Width + static_cast<std::size_t>(x)) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2]};
        }

        bool _started = false;
    };
}

TEST_F(Features445SampleTest, TheSampleRunsAndItsChecksPass)
{
    std::vector<std::string> errors;
    const auto listener = Logger::logEvent += [&errors](const std::string_view message, const Logger::LogLevel level)
    {
        if (level == Logger::LogLevel::Error)
        {
            errors.emplace_back(message);
        }
    };

    const bool ran = LuaRuntime::Instance().RunFile(IO::ResourcePath("res://main.lua"));
    if (ran)
    {
        for (int frame = 0; frame < 125; ++frame)
        {
            TickOptions tick;
            tick.pollEvents = false;
            tick.deltaSeconds = 1.0 / 60.0;
            Application::GetInstance().Tick(tick);
        }
    }
    Logger::logEvent -= listener;
    ASSERT_TRUE(ran);
    // The one error expected: the software renderer says, once, that it draws the lamp's user shader with its stand-in
    int standInNotices = 0;
    for (const std::string &error : errors)
    {
        if (error.find("pulse.n2shader") != std::string::npos &&
            error.find("cannot draw user shaders") != std::string::npos)
        {
            ++standInNotices;
            continue;
        }
        ADD_FAILURE() << "logged: " << error;
    }
    EXPECT_EQ(standInNotices, 1);

    const sol::protected_function_result result = LuaRuntime::Instance().GetState().safe_script(R"(
        local director = SceneManager.GetCurrentScene():FindGameObject('Director'):GetScript('res://scripts/Director.lua')
        return director.done == true, director.passed or 0, table.concat(director.failures or {}, '; ')
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        // Copy-initialised: MSVC finds sol::error(result) ambiguous
        const sol::error error = result;
        FAIL() << error.what();
    }
    const auto [done, passed, failures] = result.get<std::tuple<bool, int, std::string>>();
    EXPECT_TRUE(done);
    EXPECT_EQ(failures, "");
    EXPECT_EQ(passed, 14);

    // The HUD, laid out for 400 x 300 and drawn at twice that
    auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
    renderer->ReadFramebuffer(pixels.data(), Width, Height);
    const Rgb stripe = PixelAt(pixels, 160, 460);
    EXPECT_TRUE(stripe.r > 180 && stripe.g > 130 && stripe.b < 90)
        << "the stripe at the turned panel's middle: " << stripe.r << "," << stripe.g << "," << stripe.b;
    const Rgb panel = PixelAt(pixels, 140, 495);
    EXPECT_TRUE(panel.b > 110 && panel.r < 90) << "the turned panel: " << panel.r << "," << panel.g << "," << panel.b;
    const Rgb masked = PixelAt(pixels, 326, 556);
    EXPECT_FALSE(masked.r > 180 && masked.g > 130 && masked.b < 90) << "the stripe is cut at the panel's edge";
    const Rgb options = PixelAt(pixels, 530, 448);
    const Rgb quit = PixelAt(pixels, 530, 376);
    EXPECT_GT(options.b, options.r + 40) << "the selected button: " << options.r << "," << options.g << "," << options.b;
    EXPECT_LT(std::abs(quit.b - quit.r), 10) << "an unselected one: " << quit.r << "," << quit.g << "," << quit.b;
}

#endif
