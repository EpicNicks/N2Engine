#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

#include "common/BlendReference.hpp"
#include "common/CrossBackendDiff.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// Golden-image tests of UI Image sprites on the WebGPU backend (#43, stage D): tests/engine/rendering/
// SoftwareImageGoldenTests.cpp (both tests) and the two Image-only UI cases of SoftwareTextGoldenTests.cpp
// (SoftwareUIGoldenTest.AnImageFillsItsRect and APressedButtonDrawsItsImageWithThePressedTint), with the same scenes, sizes and
// structural expectations, drawn by the UI pass (UISystem::Render: a SetViewProjection in the middle of the frame, then
// every graphic with UISystem::OverlayState: no depth test or write, no cull, BLENDED) on a WebGPU device (Mesa lavapipe in
// CI; no window, the offscreen target) and read back. The sprite textures come through the engine's texture caching
// (GpuCache) as they do in the editor; Image.cpp's failed-texture memo is untouched.
//
// Cross-backend comparison. The software renderer cannot blend, so a blended frame is never diffed with one software frame:
// the reference is BlendReference.hpp's COMPOSITE: the software renderer draws the sprite opaque (its colours) and the
// same rect flat white (the coverage), and the WebGPU frame is compared with those blended over the clear colour by GL's
// equation. For the sprites whose alpha is 1 that equals the software frame (blending with alpha 1 replaces); the
// translucent variants (WebGPU only) show the equation. Nearest-sampled scenes use the STRICT mode (2/255); the linear
// gradient uses the LIT mode (mean <= 2/255, <= 1% over 8/255, a 1-pixel coverage-edge border from flat masks).
//
// The WebGPU-only scene test: a translucent Image over a scene mesh. The mesh is drawn before the UI pass by the scene's
// camera and must stay where that camera puts it (the UI pass's SetViewProjection must not move it); the Image blends over
// it by GL's equation. There the opaque right half (no blended draw) is compared strictly with the software frame and the
// blended half is checked at interior pixels against the equation (the opaque-mask approach of BlendReference.hpp).

using namespace N2Engine;
using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Software::SoftwareRenderer;

namespace
{
    /// A 24-bit BMP file of `width` x `height` pixels, given top row first (as an image editor shows them). The file stores
    /// them bottom-up, as BMPs usually do. (The software tests' TextureTestSupport::MakeBmp, which lives under tests/engine.)
    std::vector<std::uint8_t> MakeBmp(const int width, const int height,
                                      const std::vector<std::array<std::uint8_t, 3>>& topRowFirst)
    {
        const std::size_t rowBytes = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t{3};
        const std::size_t pixelBytes = rowBytes * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> bmp;
        const auto u16 = [&bmp](const std::uint32_t v)
        {
            bmp.push_back(static_cast<std::uint8_t>(v));
            bmp.push_back(static_cast<std::uint8_t>(v >> 8));
        };
        const auto u32 = [&bmp](const std::uint32_t v)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                bmp.push_back(static_cast<std::uint8_t>(v >> shift));
            }
        };
        bmp.push_back('B');
        bmp.push_back('M');
        u32(static_cast<std::uint32_t>(54 + pixelBytes)); // file size
        u32(0);                                          // reserved
        u32(54);                                         // pixel data offset
        u32(40);                                         // BITMAPINFOHEADER
        u32(static_cast<std::uint32_t>(width));
        u32(static_cast<std::uint32_t>(height));         // positive: bottom-up rows
        u16(1);                                          // planes
        u16(24);                                         // bits per pixel
        u32(0);                                          // BI_RGB
        u32(static_cast<std::uint32_t>(pixelBytes));
        u32(2835);
        u32(2835);
        u32(0);
        u32(0);
        for (int y = height - 1; y >= 0; --y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto& p = topRowFirst[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                            static_cast<std::size_t>(x)];
                bmp.push_back(p[2]); // BGR
                bmp.push_back(p[1]);
                bmp.push_back(p[0]);
            }
            for (std::size_t pad = static_cast<std::size_t>(width) * 3; pad < rowBytes; ++pad)
            {
                bmp.push_back(0);
            }
        }
        return bmp;
    }

    Diff::Image AsImage(const Frame& frame)
    {
        return Diff::Image{frame.rgba, frame.width, frame.height};
    }

    /// A full-screen Image showing `sprite` (none: a flat fill) in `colour`, drawn by the UI pass alone (as
    /// Application::Render runs it after the scene) on `renderer` (already initialized at width x height) and read back
    Frame RenderSpriteOn(Renderer::Common::IRenderer& renderer, const std::shared_ptr<Rendering::Texture>& sprite,
                         const Common::Color& colour, const int width, const int height)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4), width,
                    height};
        {
            auto scene = Scene::Create("WebGPUImageGolden");
            const auto canvas = UI::UISystem::CreateCanvas("Canvas");
            scene->AddRootGameObject(canvas);
            const auto element = UI::UISystem::CreateElement("Sprite");
            element->GetComponent<UI::RectTransform>()->StretchToParent();
            auto* image = element->AddComponent<UI::Image>();
            image->SetColor(colour);
            image->SetSprite(sprite);
            canvas->AddChild(element, false);

            renderer.BeginFrame();
            UI::UISystem::Render(*scene, &renderer, Vector2i{width, height});
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), width, height);
            scene.reset(); // the scene and its UI (and the sprite's GPU texture) go before the renderer does
        }
        return frame;
    }

    Frame RenderSpriteOnSoftware(const std::shared_ptr<Rendering::Texture>& sprite, const Common::Color& colour,
                                 const int width, const int height)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(width), static_cast<uint32_t>(height)));
        const Frame frame = RenderSpriteOn(renderer, sprite, colour, width, height);
        renderer.Shutdown();
        return frame;
    }

    class WebGpuImageGoldenTest : public WebGpuTest
    {
    protected:
        void SetUp() override
        {
            WebGpuTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
        }

        Frame RenderSprite(const std::shared_ptr<Rendering::Texture>& sprite, const int width, const int height,
                           const Common::Color& colour = Common::Color::White)
        {
            EXPECT_TRUE(R().SetRenderTargetSize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
            return RenderSpriteOn(R(), sprite, colour, width, height);
        }

        /// The GL-semantics reference of a full-screen sprite of alpha `alpha`: the software renderer's opaque sprite
        /// blended by GL's equation over its clear colour, where the rect covers (a flat render of the same rect is the
        /// coverage). `flat` is that flat software frame, kept for the coverage masks of a lit comparison.
        Diff::Image ReferenceFor(const std::shared_ptr<Rendering::Texture>& sprite, const int width, const int height,
                                 const double alpha, Frame* flat = nullptr)
        {
            const Frame under = RenderSpriteOnSoftware(nullptr, Common::Color{0.0f, 0.0f, 0.0f, 0.0f}, width, height);
            const Frame opaqueSprite = RenderSpriteOnSoftware(sprite, Common::Color::White, width, height);
            const Frame flatWhite = RenderSpriteOnSoftware(nullptr, Common::Color::White, width, height);
            Blend::Layer layer;
            layer.colour = AsImage(opaqueSprite);
            layer.covered = Blend::CoveredPixels(AsImage(flatWhite), {0, 0, 0});
            layer.alpha = alpha;
            if (flat != nullptr)
            {
                *flat = flatWhite;
            }
            return Blend::Composite(AsImage(under), layer);
        }

        /// WebGPU against the reference, strict; the measured numbers printed and recorded
        void ExpectStrictAgreement(const Frame& webgpu, const Diff::Image& reference, const std::string& what)
        {
            const Diff::Options options;
            EXPECT_TRUE(Diff::Agree(AsImage(webgpu), reference, options)) << what;
            const std::string described = Diff::Compare(AsImage(webgpu), reference, options).Describe();
            std::cout << "[ cross-diff ] " << what << " (strict): " << described << std::endl;
            Diff::RecordedLines().push_back("image golden, " + what + " (strict): " + described);
        }
    };

    constexpr Rgb kCyan{0, 255, 255};
}

TEST_F(WebGpuImageGoldenTest, ASpriteFromAnImageFileShowsUpright)
{
    // The source image, as an editor shows it: red | green over blue | yellow. BMP stores it bottom-up; the
    // decoder (flipY on, the default) stores it bottom row first, so v = 0 (the bottom of the rect) is its
    // bottom row.
    const auto bmp = MakeBmp(2, 2, {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}});
    Rendering::TextureSettings settings;
    settings.filter = TextureFilter::Nearest;
    const auto sprite = Rendering::Texture::CreateFromEncoded(bmp, settings, "checker");
    ASSERT_NE(sprite, nullptr);

    constexpr int width = 40;
    constexpr int height = 40;
    const Frame frame = RenderSprite(sprite, width, height);

    // Quadrant centres; frame y counts up from the bottom of the screen
    EXPECT_EQ(frame.At(10, 30), kRed) << "the source's top-left is at the top left";
    EXPECT_EQ(frame.At(30, 30), kGreen) << "top right";
    EXPECT_EQ(frame.At(10, 10), kBlue) << "bottom left";
    EXPECT_EQ(frame.At(30, 10), kYellow) << "bottom right";

    // Nearest: each quadrant is one solid colour, give or take the pixel on its edge
    int wrong = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (x == 19 || x == 20 || y == 19 || y == 20 || x == 0 || y == 0 || x == width - 1 || y == height - 1)
            {
                continue; // the boundary between texels, and the rect's edge pixels
            }
            const Rgb expected = y >= 20 ? (x < 20 ? kRed : kGreen) : (x < 20 ? kBlue : kYellow);
            wrong += frame.At(x, y) == expected ? 0 : 1;
        }
    }
    EXPECT_EQ(wrong, 0) << "pixels off their quadrant's colour";

    // Alpha 1: blending replaces, so the reference is the software frame; strict
    ExpectStrictAgreement(frame, ReferenceFor(sprite, width, height, 1.0), "ASpriteFromAnImageFileShowsUpright");
}

TEST_F(WebGpuImageGoldenTest, ALinearSpriteIsFilteredIntoAGradient)
{
    // Two texels, red then blue, clamped at the edges. Texel centres are a quarter and three quarters across.
    TextureOptions linear;
    linear.filter = TextureFilter::Linear;
    linear.wrap = TextureWrap::ClampToEdge;
    linear.mipmaps = false;
    const std::vector<std::uint8_t> pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    const auto sprite = Rendering::Texture::Create(2, 1, pixels, linear);
    ASSERT_NE(sprite, nullptr);

    constexpr int width = 64;
    constexpr int height = 8;
    const Frame frame = RenderSprite(sprite, width, height);
    constexpr int row = height / 2;

    // Pure colours outside the texel centres (clamped), a blend between them
    EXPECT_EQ(frame.At(2, row), kRed);
    EXPECT_EQ(frame.At(width - 3, row), kBlue);
    const Rgb middle = frame.At(width / 2, row);
    EXPECT_NEAR(middle.r, 128, 12) << "halfway: half red";
    EXPECT_NEAR(middle.b, 128, 12) << "halfway: half blue";
    EXPECT_EQ(middle.g, 0);

    // Red falls and blue rises steadily from one texel centre to the other
    std::set<std::tuple<int, int, int>> distinct;
    for (int x = width / 4; x + 1 < 3 * width / 4; ++x)
    {
        const Rgb here = frame.At(x, row);
        const Rgb next = frame.At(x + 1, row);
        EXPECT_GE(here.r, next.r) << x;
        EXPECT_LE(here.b, next.b) << x;
        EXPECT_NEAR(static_cast<int>(here.r) + static_cast<int>(here.b), 255, 2) << x << ": the weights sum to one";
        distinct.emplace(here.r, here.g, here.b);
    }
    EXPECT_GE(distinct.size(), 20u) << "a gradient, not two flat halves";

    // The lit criterion against the reference (alpha 1: the software frame), with flat masks for the coverage edges
    Frame flatSoftware;
    const Diff::Image reference = ReferenceFor(sprite, width, height, 1.0, &flatSoftware);
    const Frame flatWebGPU = RenderSprite(nullptr, width, height);
    const Diff::Image maskA = AsImage(flatWebGPU);
    const Diff::Image maskB = AsImage(flatSoftware);
    Diff::Options options = Diff::Lit();
    options.coverageA = &maskA;
    options.coverageB = &maskB;
    EXPECT_TRUE(Diff::Agree(AsImage(frame), reference, options));
    const std::string described = Diff::Compare(AsImage(frame), reference, options).Describe();
    std::cout << "[ cross-diff ] ALinearSpriteIsFilteredIntoAGradient (lit): " << described << std::endl;
    Diff::RecordedLines().push_back("image golden, ALinearSpriteIsFilteredIntoAGradient (lit): " + described);

    // The same texture sampled Nearest has only its two colours
    TextureOptions nearest = linear;
    nearest.filter = TextureFilter::Nearest;
    const Frame flat = RenderSprite(Rendering::Texture::Create(2, 1, pixels, nearest), width, height);
    for (int x = 1; x + 1 < width; ++x)
    {
        const Rgb pixel = flat.At(x, row);
        EXPECT_TRUE(pixel == kRed || pixel == kBlue) << x;
    }
    EXPECT_EQ(flat.At(width / 4, row), kRed);
    EXPECT_EQ(flat.At(3 * width / 4, row), kBlue);
}

TEST_F(WebGpuImageGoldenTest, ATranslucentSpriteBlendsWithTheClearColourByGLsEquation)
{
    // WebGPU only (the software renderer cannot blend): the checker of the upright test with an Image colour of alpha 0.5,
    // so every sprite pixel is half of its colour over the black clear colour: strict against the GL-semantics composite
    const auto bmp = MakeBmp(2, 2, {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}});
    Rendering::TextureSettings settings;
    settings.filter = TextureFilter::Nearest;
    const auto sprite = Rendering::Texture::CreateFromEncoded(bmp, settings, "checker");
    ASSERT_NE(sprite, nullptr);

    constexpr int width = 40;
    constexpr int height = 40;
    const Frame frame = RenderSprite(sprite, width, height, Common::Color{1.0f, 1.0f, 1.0f, 0.5f});
    const auto half = [](const std::uint8_t channel) { return Blend::BlendChannel(channel, 0, 0.5, false); };
    const auto expectPixel = [&](const int x, const int y, const Rgb source, const std::string& where)
    {
        const Rgb p = frame.At(x, y);
        EXPECT_NEAR(p.r, half(source.r), 1) << where;
        EXPECT_NEAR(p.g, half(source.g), 1) << where;
        EXPECT_NEAR(p.b, half(source.b), 1) << where;
    };
    expectPixel(10, 30, kRed, "top left");
    expectPixel(30, 30, kGreen, "top right");
    expectPixel(10, 10, kBlue, "bottom left");
    expectPixel(30, 10, kYellow, "bottom right");
    EXPECT_EQ(frame.Count(kRed), 0) << "nothing is drawn opaque";

    ExpectStrictAgreement(frame, ReferenceFor(sprite, width, height, 0.5), "a translucent checker sprite");
}

// ============================================================================
// The Image-only cases of SoftwareUIGoldenTest (SoftwareTextGoldenTests.cpp)
// ============================================================================

TEST_F(WebGpuImageGoldenTest, AnImageFillsItsRect)
{
    constexpr int width = 64;
    constexpr int height = 48;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);

    auto scene = Scene::Create("WebGPUUIGolden_Image");
    const auto canvas = UI::UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UI::UISystem::CreateElement("Panel");
    auto* rectTransform = element->GetComponent<UI::RectTransform>();
    ASSERT_NE(rectTransform, nullptr);
    // A 20 x 10 rect with its bottom-left corner at (8, 4), canvas space (y up)
    rectTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetAnchorMax(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetPivot(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetAnchoredPosition(Math::Vector2{8.0f, 4.0f});
    rectTransform->SetSizeDelta(Math::Vector2{20.0f, 10.0f});
    auto* image = element->AddComponent<UI::Image>();
    ASSERT_NE(image, nullptr);
    image->SetColor(Common::Color::Red);
    canvas->AddChild(element, false);

    // The UI pass on its own, as Application::Render runs it after the scene
    R().BeginFrame();
    UI::UISystem::Render(*scene, &R(), Vector2i{width, height});
    R().EndFrame();
    R().Present();
    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4), width, height};
    R().ReadFramebuffer(frame.rgba.data(), width, height);

    // Pixels that aren't the clear colour: how many, and their bounding box
    int count = 0;
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (!(frame.At(x, y) == kBlack))
            {
                ++count;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    EXPECT_NEAR(count, 200, 30) << "about 20 x 10 pixels";
    EXPECT_NEAR(minX, 8, 1);
    EXPECT_NEAR(maxX, 27, 1);
    EXPECT_NEAR(minY, 4, 1);
    EXPECT_NEAR(maxY, 13, 1);
    if (count > 0)
    {
        EXPECT_EQ(frame.At((minX + maxX) / 2, (minY + maxY) / 2), kRed) << "alpha 1 blends to exactly the colour";
    }

    scene.reset(); // the scene and its UI go before the renderer shuts down
}

TEST_F(WebGpuImageGoldenTest, APressedButtonDrawsItsImageWithThePressedTint)
{
    constexpr int width = 32;
    constexpr int height = 32;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));

    auto scene = Scene::Create("WebGPUUIGolden_Button");
    const auto canvas = UI::UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UI::UISystem::CreateElement("Button");
    element->GetComponent<UI::RectTransform>()->StretchToParent();
    auto* image = element->AddComponent<UI::Image>();
    image->SetColor(Common::Color{0.0f, 1.0f, 1.0f, 1.0f}); // cyan, so the tint shows in two channels
    auto* button = element->AddComponent<UI::Button>();
    button->SetFadeDuration(0.0f);
    button->SetPressedColor(Common::Color{0.5f, 0.5f, 0.5f, 1.0f});
    canvas->AddChild(element, false);

    const auto draw = [&]
    {
        R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
        R().BeginFrame();
        UI::UISystem::Render(*scene, &R(), Vector2i{width, height});
        R().EndFrame();
        R().Present();
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4), width, height};
        R().ReadFramebuffer(frame.rgba.data(), width, height);
        return frame.At(width / 2, height / 2);
    };

    EXPECT_EQ(draw(), kCyan) << "normal: the image's own colour";

    // Pressed with the pointer over it, as the dispatcher would send it
    button->OnMouseEnter();
    button->OnMouseDown();
    ASSERT_EQ(button->GetState(), UI::Button::State::Pressed);
    const Rgb pressed = draw();
    EXPECT_EQ(pressed.r, 0);
    EXPECT_NEAR(pressed.g, 128, 2) << "cyan times the pressed grey";
    EXPECT_NEAR(pressed.b, 128, 2);

    button->OnMouseUp();
    button->OnMouseExit();
    EXPECT_EQ(draw(), kCyan) << "released: back to the image's own colour";

    scene.reset(); // the scene and its UI go before the renderer shuts down
}

// ============================================================================
// A translucent Image over a scene mesh
// ============================================================================

namespace
{
    constexpr int kSceneSize = 64;

    /// Draws as Application::Render does: the scene pass (the camera's view and projection), then the UI pass, which sets its
    /// own view and projection in the middle of the frame. `withImage` false draws the scene alone.
    Frame RenderSceneAndUi(Renderer::Common::IRenderer& renderer, const bool withImage)
    {
        Camera camera;
        camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(Math::Vector3(0.0f, 0.0f, 3.0f));
        camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));

        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(kSceneSize) * kSceneSize * 4), kSceneSize, kSceneSize};
        {
            auto scene = Scene::Create("WebGPUImageOverMesh");
            // The scene mesh: a unit quad of opaque unlit blue at the origin. From 3 units away (45 degrees) it spans
            // pixels 19..44 across and up.
            auto gameObject = GameObject::Create("Quad");
            auto* meshRenderer = gameObject->AddComponent<Rendering::MeshRenderer>();
            meshRenderer->SetMesh(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Quad));
            auto material = Rendering::Material::Create(Rendering::ShadingModel::Unlit);
            material->SetBaseColor(Common::Color{0.0f, 0.0f, 1.0f, 1.0f});
            meshRenderer->SetMaterial(0, material);
            scene->AddRootGameObject(gameObject);
            if (withImage)
            {
                // The UI: a half-transparent red Image over the LEFT half of the screen (pixels 0..31)
                const auto canvas = UI::UISystem::CreateCanvas("Canvas");
                scene->AddRootGameObject(canvas);
                const auto element = UI::UISystem::CreateElement("Glass");
                auto* rectTransform = element->GetComponent<UI::RectTransform>();
                rectTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
                rectTransform->SetAnchorMax(Math::Vector2{0.0f, 1.0f});
                rectTransform->SetPivot(Math::Vector2{0.0f, 0.0f});
                rectTransform->SetAnchoredPosition(Math::Vector2{0.0f, 0.0f});
                rectTransform->SetSizeDelta(Math::Vector2{32.0f, 0.0f});
                element->AddComponent<UI::Image>()->SetColor(Common::Color{1.0f, 0.0f, 0.0f, 0.5f});
                canvas->AddChild(element, false);
            }

            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            scene->Render(&renderer, camera);
            if (withImage)
            {
                UI::UISystem::Render(*scene, &renderer, Vector2i{kSceneSize, kSceneSize});
            }
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), kSceneSize, kSceneSize);
            scene.reset(); // the scene and its components (and their GPU resources) go before the renderer does
        }
        return frame;
    }
}

TEST_F(WebGpuImageGoldenTest, ATranslucentImageOverASceneMeshFollowsGLsEquationAndTheMeshStaysWhereItsCameraPutsIt)
{
    ASSERT_TRUE(R().SetRenderTargetSize(kSceneSize, kSceneSize));
    const Frame sceneOnly = RenderSceneAndUi(R(), false);
    const Frame frame = RenderSceneAndUi(R(), true);

    // The mesh alone: where its camera puts it (pixels 19..44), exactly blue
    constexpr int row = kSceneSize / 2;
    EXPECT_EQ(sceneOnly.At(36, row), kBlue);
    EXPECT_EQ(sceneOnly.At(10, row), kBlack);
    const Coverage meshCoverage = CoverageOf(sceneOnly, kBlue);
    EXPECT_NEAR(meshCoverage.minX, 19, 1);
    EXPECT_NEAR(meshCoverage.maxX, 44, 1);
    EXPECT_NEAR(meshCoverage.minY, 19, 1);
    EXPECT_NEAR(meshCoverage.maxY, 44, 1);

    // With the UI pass after it: the mesh is where it was (the right half, outside the Image, is the scene alone)...
    for (int x = 34; x < kSceneSize; ++x)
    {
        for (int y = 0; y < kSceneSize; ++y)
        {
            ASSERT_EQ(frame.At(x, y), sceneOnly.At(x, y)) << "the UI pass changed the scene at (" << x << "," << y << ")";
        }
    }
    // ...and the Image blends over it by GL's equation: over the mesh's blue, and over the black clear colour
    const std::array<int, 3> overBlue = {Blend::BlendChannel(255, 0, 0.5, false), 0, Blend::BlendChannel(0, 255, 0.5, false)};
    const std::array<int, 3> overBlack = {Blend::BlendChannel(255, 0, 0.5, false), 0, 0};
    for (const int x : {22, 26, 30})
    {
        const Rgb p = frame.At(x, row);
        EXPECT_NEAR(p.r, overBlue[0], 1) << "the Image over the mesh, x " << x;
        EXPECT_EQ(p.g, 0);
        EXPECT_NEAR(p.b, overBlue[2], 1);
    }
    for (const int x : {2, 8, 14})
    {
        const Rgb p = frame.At(x, row);
        EXPECT_NEAR(p.r, overBlack[0], 1) << "the Image over the clear colour, x " << x;
        EXPECT_EQ(p.g, 0);
        EXPECT_EQ(p.b, 0);
    }
    EXPECT_EQ(frame.Count(kRed), 0) << "the Image is translucent, nowhere opaque red";

    // The opaque part (no blended draw: the right of the Image, with a border) against the software frame, strictly. The
    // software renderer draws the Image opaque where the WebGPU frame blends it, which is why the left is left out.
    SoftwareRenderer software;
    ASSERT_TRUE(software.Initialize(nullptr, kSceneSize, kSceneSize));
    const Frame softwareFrame = RenderSceneAndUi(software, true);
    software.Shutdown();
    std::vector<bool> ignore(static_cast<std::size_t>(kSceneSize) * kSceneSize, false);
    for (int y = 0; y < kSceneSize; ++y)
    {
        for (int x = 0; x < 34; ++x)
        {
            ignore[static_cast<std::size_t>(y) * kSceneSize + static_cast<std::size_t>(x)] = true;
        }
    }
    Diff::Options options;
    options.ignore = &ignore;
    EXPECT_TRUE(Diff::Agree(AsImage(frame), AsImage(softwareFrame), options));
    const std::string described = Diff::Compare(AsImage(frame), AsImage(softwareFrame), options).Describe();
    std::cout << "[ cross-diff ] a translucent Image over a scene mesh (opaque part, strict): " << described << std::endl;
    Diff::RecordedLines().push_back("image golden, a translucent Image over a scene mesh (opaque part): " + described);
}
