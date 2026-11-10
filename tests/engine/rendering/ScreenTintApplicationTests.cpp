#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// The screen tint as Application::Render draws it (effects design, stage E2): on the scene pass only, never on the
// screen-space UI pass after it (in Unity, overlay UI is drawn after the full-screen passes, so the HUD stays in colour),
// and on the clear colour (on the CPU when the tint covers the whole frame, else by a tinted backdrop quad). A headless software application: no display or GPU.

using namespace N2Engine;
using Renderer::Common::ScreenTint;

namespace
{
    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    class ScreenTintApplicationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Config::ApplicationOptions options;
            options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            ASSERT_TRUE(window.SetRenderSize(Width, Height));
            Application::GetInstance().Set3DPhysicsBackend(nullptr);

            SceneManager::AddScene(Scene::Create("ScreenTintApplicationTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);

            // A red UI image over the left half of the screen; the right half is the clear colour, red too
            const auto canvas = UI::UISystem::CreateCanvas("Canvas");
            SceneManager::GetCurSceneRef().AddRootGameObject(canvas);
            const auto element = UI::UISystem::CreateElement("LeftHalf");
            auto *rect = element->GetComponent<UI::RectTransform>();
            rect->StretchToParent();
            rect->SetAnchorMax(Math::Vector2{0.5f, 1.0f});
            element->AddComponent<UI::Image>()->SetColor(Common::Color::Red);
            canvas->AddChild(element, false);

            window.clearColor = Common::Color::Red;
        }

        void TearDown() override
        {
            Application::GetInstance().ClearScreenTint();
            Application::GetInstance().GetWindow().clearColor = Common::Color::Black;
            SceneManager::AddScene(Scene::Create("ScreenTintApplicationTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
        }

        /// The pixel at (x, y), y counted up from the bottom, of the frame the renderer holds
        static Rgb PixelAt(const int x, const int y)
        {
            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
            renderer->ReadFramebuffer(pixels.data(), Width, Height);
            const std::size_t i = (static_cast<std::size_t>(y) * Width + static_cast<std::size_t>(x)) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2]};
        }

        static ::testing::AssertionResult Is(const Rgb &actual, const int r, const int g, const int b)
        {
            if (std::abs(actual.r - r) > 1 || std::abs(actual.g - g) > 1 || std::abs(actual.b - b) > 1)
            {
                return ::testing::AssertionFailure() << "(" << actual.r << "," << actual.g << "," << actual.b << ") is not ("
                                                     << r << "," << g << "," << b << ")";
            }
            return ::testing::AssertionSuccess();
        }

        static constexpr int Width = 32;
        static constexpr int Height = 16;
        static constexpr int LeftX = 4;   // under the UI image
        static constexpr int RightX = 28; // the clear colour
        static inline int sceneCounter = 0;
    };
}

TEST_F(ScreenTintApplicationTest, WithoutATintBothHalvesAreRed)
{
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(LeftX, Height / 2), 255, 0, 0));
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 255, 0, 0));
}

TEST_F(ScreenTintApplicationTest, AWholeScreenTintGreysTheClearColourButNotTheUi)
{
    ScreenTint grey;
    grey.saturation = 0.0f;
    Application::GetInstance().SetScreenTint(grey);
    Application::GetInstance().RenderGameFrame();

    // Red's grey is its luma, 0.299 (the clear colour is truncated to 8 bits: 76)
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 76, 76, 76)) << "the clear colour, tinted on the CPU";
    EXPECT_TRUE(Is(PixelAt(LeftX, Height / 2), 255, 0, 0)) << "the UI pass is drawn in its own colours";
    EXPECT_FALSE(Application::GetInstance().GetWindow().GetRenderer()->GetScreenTint().IsActive())
        << "the renderer's tint is cleared after the frame";
    EXPECT_FALSE(Application::GetInstance().GetScreenTint() == ScreenTint{}) << "the game's tint stays for the next frame";
}

TEST_F(ScreenTintApplicationTest, APartialCircleTintsTheBareClearColourInsideItOnly)
{
    // A circle of 4 pixels (half a half-height of 16) round the middle (16, 8): the UI image covers x 0 to 15, so the
    // pixels tested are the bare clear colour, under the backdrop quad Application draws for a partial circle
    ScreenTint circle;
    circle.saturation = 0.0f;
    circle.radius = 0.5f;
    Application::GetInstance().SetScreenTint(circle);
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(Width / 2 + 1, Height / 2), 76, 76, 76)) << "the clear colour inside the circle";
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 255, 0, 0)) << "the clear colour outside the circle";
    EXPECT_TRUE(Is(PixelAt(LeftX, Height / 2), 255, 0, 0)) << "the UI pass is drawn in its own colours";
}

TEST_F(ScreenTintApplicationTest, ATintOutsideACircleTintsTheClearColourAroundIt)
{
    ScreenTint circle;
    circle.saturation = 0.0f;
    circle.radius = 0.5f;
    circle.inside = false;
    Application::GetInstance().SetScreenTint(circle);
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(Width / 2 + 1, Height / 2), 255, 0, 0)) << "inside the circle: as drawn";
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 76, 76, 76)) << "outside the circle: grey";
}

TEST_F(ScreenTintApplicationTest, ClearingTheTintBringsTheColourBack)
{
    ScreenTint grey;
    grey.saturation = 0.0f;
    Application::GetInstance().SetScreenTint(grey);
    Application::GetInstance().RenderGameFrame();
    Application::GetInstance().ClearScreenTint();
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 255, 0, 0));
}
