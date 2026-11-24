#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/common/PostProcess.hpp>
#include <renderer/software/SWTexture.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/rendering/PostProcessStack.hpp"
#include "engine/rendering/RenderTexture.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "../modules/ModuleGuards.hpp"

// The PostProcessStack and render-texture captures as Application::Render runs them (issue #445): after the scene pass and before
// the screen-space UI, so the HUD is neither processed nor captured. A headless software application: no display or GPU.

using namespace N2Engine;
using Renderer::Common::PostProcessSettings;

namespace
{
    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    class PostProcessApplicationTest : public ::testing::Test
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

            SceneManager::AddScene(Scene::Create("PostProcessApplicationTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);

            // A red UI image over the left half; the rest is the black clear colour
            const auto canvas = UI::UISystem::CreateCanvas("Canvas");
            SceneManager::GetCurSceneRef().AddRootGameObject(canvas);
            const auto element = UI::UISystem::CreateElement("LeftHalf");
            auto *rect = element->GetComponent<UI::RectTransform>();
            rect->StretchToParent();
            rect->SetAnchorMax(Math::Vector2{0.5f, 1.0f});
            element->AddComponent<UI::Image>()->SetColor(Common::Color::Red);
            canvas->AddChild(element, false);

            auto effects = GameObject::Create("Effects");
            stack = effects->AddComponent<Rendering::PostProcessStack>();
            SceneManager::GetCurSceneRef().AddRootGameObject(effects);
            SceneManager::GetCurSceneRef().ProcessAttachQueue(); // the scene finds the stack once it is attached
            window.clearColor = Common::Color::Black;
            Application::GetInstance().ClearPendingCaptures();
        }

        void TearDown() override
        {
            stack = nullptr;
            SceneManager::AddScene(Scene::Create("PostProcessApplicationTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
        }

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

        /// The texel (x, y) (y up) of a render texture on the software renderer
        static Rgb TexelAt(const Rendering::RenderTexture &texture, const int x, const int y)
        {
            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            const auto *software = dynamic_cast<const Renderer::Software::SWTexture *>(texture.GetGpuTexture(*renderer));
            if (software == nullptr)
            {
                ADD_FAILURE() << "the render texture has no software texture";
                return {};
            }
            const std::size_t i = (static_cast<std::size_t>(y) * software->width + static_cast<std::size_t>(x)) * 4;
            return {software->data[i], software->data[i + 1], software->data[i + 2]};
        }

        static constexpr int Width = 32;
        static constexpr int Height = 16;
        static constexpr int LeftX = 4;   // under the UI image
        static constexpr int RightX = 28; // the clear colour
        static inline int sceneCounter = 0;
        Rendering::PostProcessStack *stack = nullptr;
    };
}

TEST_F(PostProcessApplicationTest, TheComponentIsRegisteredWithItsFields)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("PostProcessStack"));
    const auto schema = ComponentRegistry::Instance().Describe("PostProcessStack");
    ASSERT_TRUE(schema.has_value());
    for (const char *field : {"bloom", "bloomThreshold", "bloomIntensity", "bloomIterations", "bloomTint", "invert",
                              "chromaticAberration", "vignette"})
    {
        EXPECT_TRUE(schema->defaults.contains(field)) << field;
    }
}

TEST_F(PostProcessApplicationTest, AStackWithEveryEffectOffChangesNothing)
{
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 0, 0, 0));
}

TEST_F(PostProcessApplicationTest, TheSceneIsProcessedAndTheUiIsNot)
{
    N2_SKIP_WITHOUT_UI();
    stack->invert = 1.0f;
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 255, 255, 255)) << "the scene (the clear colour) is inverted";
    EXPECT_TRUE(Is(PixelAt(LeftX, Height / 2), 255, 0, 0)) << "the UI is drawn after the post-process";
}

TEST_F(PostProcessApplicationTest, AnInactiveStackIsIgnored)
{
    stack->invert = 1.0f;
    stack->SetActive(false);
    Application::GetInstance().RenderGameFrame();
    EXPECT_TRUE(Is(PixelAt(RightX, Height / 2), 0, 0, 0));
}

TEST_F(PostProcessApplicationTest, SettingsRoundTrip)
{
    PostProcessSettings settings;
    settings.bloom = true;
    settings.bloomThreshold = 0.25f;
    settings.bloomIterations = 6;
    settings.bloomTint[1] = 0.5f;
    settings.vignette = 0.3f;
    stack->SetSettings(settings);
    const PostProcessSettings back = stack->GetSettings();
    EXPECT_TRUE(back.bloom);
    EXPECT_FLOAT_EQ(back.bloomThreshold, 0.25f);
    EXPECT_EQ(back.bloomIterations, 6);
    EXPECT_FLOAT_EQ(back.bloomTint[1], 0.5f);
    EXPECT_FLOAT_EQ(back.vignette, 0.3f);
}

TEST_F(PostProcessApplicationTest, ARequestedCaptureHoldsTheProcessedSceneWithoutTheUiOnce)
{
    N2_SKIP_WITHOUT_UI();
    stack->invert = 1.0f;
    const auto texture = Rendering::RenderTexture::Create(8, 4);
    ASSERT_NE(texture, nullptr);
    EXPECT_TRUE(texture->GetTexture()->IsRenderTarget());
    EXPECT_TRUE(texture->GetTexture()->IsLoaded());
    Application::GetInstance().RequestCapture(texture);
    Application::GetInstance().RequestCapture(texture); // asked twice: captured once
    EXPECT_EQ(Application::GetInstance().GetPendingCaptureCount(), 1u);
    Application::GetInstance().RenderGameFrame();
    EXPECT_EQ(Application::GetInstance().GetPendingCaptureCount(), 0u);
    EXPECT_EQ(texture->GetCaptureCount(), 1u);
    EXPECT_TRUE(Is(TexelAt(*texture, 1, 2), 255, 255, 255)) << "the left half holds the scene, not the red UI";
    EXPECT_TRUE(Is(TexelAt(*texture, 6, 2), 255, 255, 255));
    Application::GetInstance().RenderGameFrame();
    EXPECT_EQ(texture->GetCaptureCount(), 1u) << "a request is captured once";
}

TEST_F(PostProcessApplicationTest, ACameraTargetTextureIsCapturedEveryFrame)
{
    const auto texture = Rendering::RenderTexture::Create(4, 4);
    ASSERT_NE(texture, nullptr);
    Camera camera;
    camera.SetTargetTexture(texture);
    Application::GetInstance().RenderEditorFrame(camera);
    Application::GetInstance().RenderEditorFrame(camera);
    EXPECT_EQ(texture->GetCaptureCount(), 2u);
    camera.SetTargetTexture(nullptr);
    Application::GetInstance().RenderEditorFrame(camera);
    EXPECT_EQ(texture->GetCaptureCount(), 2u);
}

TEST(RenderTextureTest, BadSizesAreRefused)
{
    EXPECT_EQ(Rendering::RenderTexture::Create(0, 4), nullptr);
    EXPECT_EQ(Rendering::RenderTexture::Create(4, Renderer::Common::MaxRenderTextureSize + 1), nullptr);
    EXPECT_EQ(Rendering::Texture::CreateRenderTarget(0, 0), nullptr);
}
