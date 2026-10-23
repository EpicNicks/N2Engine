#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Texture.hpp"

#include "common/BlendReference.hpp"
#include "common/CrossBackendDiff.hpp"
#include "WebGpuTestFixture.hpp"

// Golden-image tests of MeshRenderer on the WebGPU backend (#155, stage G3b), mirroring tests/renderer/vulkan/VulkanMeshGoldenTests.cpp
// (#43): the unlit scenes and the lit one of
// tests/engine/rendering/SoftwareMeshGoldenTests.cpp, with the same scenes, cameras, sizes and structural expectations
// (colours in screen regions, never exact images): scenes drawn by Scene::Render on a WebGPU device (Mesa lavapipe in CI;
// no window, the offscreen target) and read back. A textured cube must show its texture upright on the faces the camera
// sees (so a flipped UV, a swapped axis or a wrong winding fails), a cutout material must leave holes, a double-sided quad
// must show from behind and a single-sided one must not, and two submeshes must show their two materials, and (stage C) a
// lit face toward the light must be brighter than one edge-on to it. The two-submesh test's blended "mixed" part (stage D)
// has a WebGPU expectation of its own: the software renderer cannot blend, so that frame is compared with it on the opaque
// half only (BlendReference.hpp). A scene that only a GPU backend can draw proves that the Transparent queue reaches the target back to
// front through Scene::Render: two overlapping half-alpha quads, compared with the CPU composite of their software layers.
//
// Every scene is also rendered on a headless SoftwareRenderer and the two frames are compared pixel by pixel (the cross
// backend diff, CrossBackendDiff.hpp): the unlit scenes are nearest-sampled, so the software renderer is exact and the
// proposal's strict threshold applies, |WebGPU - Software| <= 2/255 per channel; the lit scene uses the lit criterion (mean
// error <= 2/255, <= 1% of the pixels over 8/255, a 1-pixel coverage-edge border).
//
// The software tests are untouched: the helpers below are copies of that file's.

using namespace N2Engine;
using Rendering::AlphaMode;
using Rendering::BuiltinMesh;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::ShadingModel;
using Rendering::Submesh;
using Rendering::Texture;
using Renderer::Common::MeshData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::Vertex;
using Renderer::Software::SoftwareRenderer;
using Renderer::WebGpu::WebGpuRenderer;
using WebGpuTesting::WebGpuTest;

namespace
{
    constexpr int Size = 64; // square frames

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb&) const = default;
        [[nodiscard]] int Sum() const { return r + g + b; }
    };

    constexpr Rgb kBlack{0, 0, 0};
    constexpr Rgb kRed{255, 0, 0};
    constexpr Rgb kGreen{0, 255, 0};
    constexpr Rgb kBlue{0, 0, 255};
    constexpr Rgb kYellow{255, 255, 0};

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] int Count(const Rgb colour) const
        {
            int count = 0;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    count += At(x, y) == colour ? 1 : 0;
                }
            }
            return count;
        }

        [[nodiscard]] VulkanTesting::Diff::Image AsImage() const { return {rgba, width, height}; }
    };

    /// A square perspective camera (45 degrees) at `position`, looking at the origin with y up
    Camera CameraAt(const Math::Vector3& position)
    {
        Camera camera;
        camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(position);
        camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
        return camera;
    }

    /// Builds a scene and draws it as Application::Render does for the scene pass, on `renderer` (already initialized at
    /// Size x Size), and reads it back
    Frame RenderOn(Renderer::Common::IRenderer& renderer, const std::function<void(Scene&)>& build, const Camera& camera,
                   const Renderer::Common::SceneLightingData* lighting = nullptr)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4), Size, Size};
        {
            auto scene = Scene::Create("WebGPUMeshGolden");
            build(*scene);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            if (lighting != nullptr)
            {
                renderer.UpdateSceneLighting(*lighting, camera.GetPosition());
            }
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset(); // the scene and its components (and their GPU resources) go before the renderer does
        }
        return frame;
    }

    /// The same scene on a fresh headless software renderer: the reference for the cross-backend diff
    Frame RenderOnSoftware(const std::function<void(Scene&)>& build, const Camera& camera,
                           const Renderer::Common::SceneLightingData* lighting = nullptr)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        const Frame frame = RenderOn(renderer, build, camera, lighting);
        renderer.Shutdown();
        return frame;
    }

    /// A MeshRenderer on a new root object at `position`
    MeshRenderer* AddMesh(Scene& scene, const std::string& name, std::shared_ptr<Mesh> mesh,
                          const Math::Vector3& position = Math::Vector3(0.0f, 0.0f, 0.0f))
    {
        auto gameObject = GameObject::Create(name);
        auto* renderer = gameObject->AddComponent<MeshRenderer>();
        renderer->SetMesh(std::move(mesh));
        gameObject->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(gameObject);
        return renderer;
    }

    std::shared_ptr<Material> Unlit(const Common::Color& colour)
    {
        auto material = Material::Create(ShadingModel::Unlit);
        material->SetBaseColor(colour);
        return material;
    }

    /// The 2 x 2 checker as an image editor shows it: red | green over blue | yellow. Its first row of data
    /// (v = 0) is the bottom one, as imported images are stored. Nearest, so each texel is one flat colour.
    std::shared_ptr<Texture> Checker()
    {
        TextureOptions nearest;
        nearest.filter = TextureFilter::Nearest;
        nearest.wrap = Renderer::Common::TextureWrap::ClampToEdge;
        nearest.mipmaps = false;
        const std::vector<std::uint8_t> pixels = {
            0, 0, 255, 255, /* blue */ 255, 255, 0, 255, /* yellow */ // bottom row (v = 0)
            255, 0, 0, 255, /* red */ 0, 255, 0, 255,    /* green */  // top row
        };
        return Texture::Create(2, 2, pixels, nearest);
    }

    /// Expects the checker upright in the square of side `half * 2` pixels around the frame's centre:
    /// red top left, green top right, blue bottom left, yellow bottom right
    void ExpectCheckerUpright(const Frame& frame, const int half, const std::string& view)
    {
        constexpr int c = Size / 2;
        const int q = half / 2; // quadrant centres
        EXPECT_EQ(frame.At(c - q, c + q), kRed) << view << ": the texture's top left is at the top left";
        EXPECT_EQ(frame.At(c + q, c + q), kGreen) << view << ": top right";
        EXPECT_EQ(frame.At(c - q, c - q), kBlue) << view << ": bottom left";
        EXPECT_EQ(frame.At(c + q, c - q), kYellow) << view << ": bottom right";
        EXPECT_EQ(frame.At(2, 2), kBlack) << view << ": the corner of the frame is background";
        // Every quadrant is a good share of the face
        for (const Rgb colour : {kRed, kGreen, kBlue, kYellow})
        {
            EXPECT_GT(frame.Count(colour), (half - 2) * (half - 2) * 3 / 4) << view;
        }
    }

    /// The fixture: a WebGPU renderer with validation on at Size x Size, and the compiled shaders required. Renders a scene
    /// on it and on the software renderer and checks the two against each other.
    class WebGpuMeshGoldenTest : public WebGpuTest
    {
    protected:
        void SetUp() override
        {
            WebGpuTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));
        }

        /// The scene on WebGPU, with the cross-backend comparison against the software renderer. `options` are the diff's
        /// (the default: the proposal's strict 2/255 per channel, every pixel)
        Frame Render(const std::function<void(Scene&)>& build, const Camera& camera, const std::string& what,
                     VulkanTesting::Diff::Options options = {},
                     const Renderer::Common::SceneLightingData* lighting = nullptr,
                     const std::function<void(Scene&)>& maskBuild = nullptr)
        {
            const Frame webgpu = RenderOn(R(), build, camera, lighting);
            const Frame software = RenderOnSoftware(build, camera, lighting);
            // A lit comparison takes its coverage edges from the same geometry drawn flat unlit (`maskBuild`) on each
            // backend, never from the lit pixels
            Frame webgpuMask;
            Frame softwareMask;
            VulkanTesting::Diff::Image maskA;
            VulkanTesting::Diff::Image maskB;
            if (maskBuild)
            {
                webgpuMask = RenderOn(R(), maskBuild, camera, nullptr);
                softwareMask = RenderOnSoftware(maskBuild, camera, nullptr);
                maskA = webgpuMask.AsImage();
                maskB = softwareMask.AsImage();
                options.coverageA = &maskA;
                options.coverageB = &maskB;
            }
            else
            {
                EXPECT_EQ(options.maxMeanError, 0.0) << what << ": a lit comparison needs a coverage mask scene";
            }
            EXPECT_TRUE(VulkanTesting::Diff::Agree(webgpu.AsImage(), software.AsImage(), options)) << what;
            // The measured numbers, for the log (the PR body quotes them)
            const std::string described = VulkanTesting::Diff::Compare(webgpu.AsImage(), software.AsImage(), options).Describe();
            std::cout << "[ cross-diff ] " << what << ": " << described << std::endl;
            VulkanTesting::Diff::RecordedLines().push_back("mesh golden, " + what + ": " + described);
            return webgpu;
        }
    };
}

TEST_F(WebGpuMeshGoldenTest, ATexturedCubeShowsItsCheckerUprightOnTheFacesTheCameraSees)
{
    const auto texture = Checker();
    ASSERT_NE(texture, nullptr);
    const auto build = [&texture](Scene& scene)
    {
        auto material = Material::Create(ShadingModel::Unlit);
        material->SetBaseColorTexture(texture);
        AddMesh(scene, "Cube", Mesh::GetBuiltin(BuiltinMesh::Cube))->SetMaterial(0, material);
    };

    // The front face (+Z, the side objects face) from a camera on +Z: a face 1 unit wide, 2.5 units away, fills
    // about 31 of the 64 pixels across (45 degrees)
    const Frame front = Render(build, CameraAt(Math::Vector3(0.0f, 0.0f, 3.0f)), "front");
    ExpectCheckerUpright(front, 15, "front");

    // The right face (+X) from a camera on +X: u runs from +Z (the screen's left there) to -Z, v up the y axis
    const Frame right = Render(build, CameraAt(Math::Vector3(3.0f, 0.0f, 0.0f)), "right");
    ExpectCheckerUpright(right, 15, "right");

    // The back face (-Z) from behind: the same picture again, as each face is mapped looking at it from outside
    const Frame back = Render(build, CameraAt(Math::Vector3(0.0f, 0.0f, -3.0f)), "back");
    ExpectCheckerUpright(back, 15, "back");
}

TEST_F(WebGpuMeshGoldenTest, ALitFaceTowardTheLightIsBrighterThanOneAwayFromIt)
{
    // A directional light shining down -Z: the front face (+Z) faces it, the right face (+X) is edge-on to it.
    // The camera, at 45 degrees between them, sees the front face left of centre and the right face right of it.
    Renderer::Common::SceneLightingData lighting;
    Renderer::Common::DirectionalLightData sun;
    sun.direction = Math::Vector3(0.0f, 0.0f, -1.0f);
    lighting.directionalLights.push_back(sun);
    const Camera camera = CameraAt(Math::Vector3(2.5f, 0.0f, 2.5f));
    constexpr int frontX = 23;
    constexpr int rightX = 41;
    constexpr int row = Size / 2;

    // Lit (MeshRenderer's default material), against the software renderer with the lit criterion
    const Frame lit = Render([](Scene& scene) { AddMesh(scene, "LitCube", Mesh::GetBuiltin(BuiltinMesh::Cube)); }, camera,
                             "lit cube", VulkanTesting::Diff::Lit(), &lighting, [](Scene& scene)
    {
        AddMesh(scene, "MaskCube", Mesh::GetBuiltin(BuiltinMesh::Cube))->SetMaterial(0, Unlit(Common::Color::White));
    });
    const Rgb litFront = lit.At(frontX, row);
    const Rgb litRight = lit.At(rightX, row);
    EXPECT_GT(litFront.Sum(), litRight.Sum() + 300) << "toward the light vs edge-on to it";
    EXPECT_GT(litRight.Sum(), 0) << "the ambient light still reaches the other face";
    EXPECT_EQ(litFront.r, litFront.g) << "white light on a white material stays grey";
    EXPECT_EQ(litFront.g, litFront.b);

    // Unlit: the same scene shows both faces alike (the strict comparison: nothing is lit)
    const Frame unlit = Render([](Scene& scene)
    {
        AddMesh(scene, "UnlitCube", Mesh::GetBuiltin(BuiltinMesh::Cube))->SetMaterial(0, Unlit(Common::Color::White));
    }, camera, "unlit cube under the same light", {}, &lighting);
    EXPECT_EQ(unlit.At(frontX, row), (Rgb{255, 255, 255}));
    EXPECT_EQ(unlit.At(rightX, row), (Rgb{255, 255, 255}));
}

TEST_F(WebGpuMeshGoldenTest, AnAlphaMaskMaterialLeavesHoles)
{
    // Left texel transparent (blue, alpha 0), right texel opaque green; a red quad behind
    TextureOptions nearest;
    nearest.filter = TextureFilter::Nearest;
    const auto texture = Texture::Create(2, 1, std::vector<std::uint8_t>{0, 0, 255, 0, 0, 255, 0, 255}, nearest);
    ASSERT_NE(texture, nullptr);
    const auto build = [&texture](Scene& scene)
    {
        auto cutout = Material::Create(ShadingModel::Unlit);
        cutout->SetBaseColorTexture(texture);
        cutout->SetAlphaMode(AlphaMode::Mask);
        cutout->SetAlphaCutoff(0.5f);
        AddMesh(scene, "Cutout", Mesh::GetBuiltin(BuiltinMesh::Quad))->SetMaterial(0, cutout);

        auto backdrop = GameObject::Create("Backdrop");
        auto* quad = backdrop->AddComponent<Example::QuadRenderer>();
        quad->SetColor(Common::Color::Red);
        quad->SetSize(Math::Vector3(10.0f, 10.0f, 1.0f));
        backdrop->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.0f, -1.0f));
        scene.AddRootGameObject(backdrop);
    };

    // The quad (1 unit, 2 units away) spans about 38 pixels around the centre
    const Frame frame = Render(build, CameraAt(Math::Vector3(0.0f, 0.0f, 2.0f)), "mask");
    constexpr int row = Size / 2;
    EXPECT_EQ(frame.At(Size / 2 - 10, row), kRed) << "the transparent half is a hole: the backdrop shows";
    EXPECT_EQ(frame.At(Size / 2 + 10, row), kGreen);
    EXPECT_EQ(frame.Count(kBlue), 0) << "no pixel of the transparent texel is drawn";
    EXPECT_GT(frame.Count(kGreen), 250);

    // The same texture without the mask draws both halves (no blending: the alpha is ignored)
    const Frame opaque = Render([&texture](Scene& scene)
    {
        auto plain = Material::Create(ShadingModel::Unlit);
        plain->SetBaseColorTexture(texture);
        AddMesh(scene, "Plain", Mesh::GetBuiltin(BuiltinMesh::Quad))->SetMaterial(0, plain);
    }, CameraAt(Math::Vector3(0.0f, 0.0f, 2.0f)), "no mask");
    EXPECT_EQ(opaque.At(Size / 2 - 10, row), kBlue);
    EXPECT_EQ(opaque.At(Size / 2 + 10, row), kGreen);
}

TEST_F(WebGpuMeshGoldenTest, ADoubleSidedQuadShowsFromBehindAndASingleSidedOneDoesNot)
{
    const Camera behind = CameraAt(Math::Vector3(0.0f, 0.0f, -2.0f)); // the quad faces +Z, away from it
    const auto quadWith = [](const bool doubleSided)
    {
        return [doubleSided](Scene& scene)
        {
            auto material = Unlit(Common::Color::Green);
            material->SetDoubleSided(doubleSided);
            AddMesh(scene, "Quad", Mesh::GetBuiltin(BuiltinMesh::Quad))->SetMaterial(0, material);
        };
    };

    const Frame singleSided = Render(quadWith(false), behind, "single-sided, from behind");
    EXPECT_EQ(singleSided.Count(kBlack), Size * Size) << "its back is culled";

    const Frame doubleSided = Render(quadWith(true), behind, "double-sided, from behind");
    EXPECT_EQ(doubleSided.At(Size / 2, Size / 2), kGreen);
    EXPECT_GT(doubleSided.Count(kGreen), 1000);

    // From the front both show
    const Frame front = Render(quadWith(false), CameraAt(Math::Vector3(0.0f, 0.0f, 2.0f)), "single-sided, from the front");
    EXPECT_EQ(front.At(Size / 2, Size / 2), kGreen);
}

TEST_F(WebGpuMeshGoldenTest, TwoSubmeshesShowTheirTwoMaterials)
{
    // One mesh: a left square (x -1..0) and a right square (x 0..1), one submesh each
    MeshData data;
    const auto square = [&data](const float x0)
    {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        const float x1 = x0 + 1.0f;
        data.vertices.push_back(Vertex{{x0, -0.5f, 0.0f}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}});
        data.vertices.push_back(Vertex{{x1, -0.5f, 0.0f}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}});
        data.vertices.push_back(Vertex{{x1, 0.5f, 0.0f}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}});
        data.vertices.push_back(Vertex{{x0, 0.5f, 0.0f}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}});
        for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
        {
            data.indices.push_back(base + index);
        }
    };
    square(-1.0f);
    square(0.0f);
    const auto mesh = Mesh::Create(std::move(data), {Submesh{0, 6, {}}, Submesh{6, 6, {}}});
    ASSERT_NE(mesh, nullptr);

    const Frame frame = Render([&mesh](Scene& scene)
    {
        auto* renderer = AddMesh(scene, "TwoMaterials", mesh);
        renderer->SetMaterial(0, Unlit(Common::Color::Red));
        renderer->SetMaterial(1, Unlit(Common::Color::Green));
    }, CameraAt(Math::Vector3(0.0f, 0.0f, 3.0f)), "two submeshes");

    constexpr int row = Size / 2;
    EXPECT_EQ(frame.At(Size / 2 - 8, row), kRed) << "submesh 0, left";
    EXPECT_EQ(frame.At(Size / 2 + 8, row), kGreen) << "submesh 1, right";
    const int red = frame.Count(kRed);
    const int green = frame.Count(kGreen);
    EXPECT_GT(red, 300);
    EXPECT_NEAR(red, green, red / 10) << "two halves of one size";

    // The mixed opaque and blended mesh of the software test: submesh 0 red, submesh 1 an Unlit green of alpha 0.5 with
    // AlphaMode::Blend, which the Transparent queue draws blended. The software renderer cannot blend and draws that half
    // opaque green (its test keeps kGreen); WebGPU blends like OpenGL: the left half is exactly kRed, the right half is
    // half green over the black clear colour, (0, 128, 0) within one unit. So this scene is NOT compared with the software
    // frame as a whole: the left half (the opaque part, with a 2-pixel border around the seam) is compared strictly, and
    // the blended half is checked against GL's equation at its interior pixels (BlendReference.hpp).
    const auto buildMixed = [&mesh](Scene& scene)
    {
        auto* renderer = AddMesh(scene, "Mixed", mesh);
        renderer->SetMaterial(0, Unlit(Common::Color::Red));
        auto glass = Unlit(Common::Color{0.0f, 1.0f, 0.0f, 0.5f});
        glass->SetAlphaMode(AlphaMode::Blend);
        renderer->SetMaterial(1, glass);
    };
    const Camera camera = CameraAt(Math::Vector3(0.0f, 0.0f, 3.0f));
    const Frame mixed = RenderOn(R(), buildMixed, camera);
    const Frame mixedSoftware = RenderOnSoftware(buildMixed, camera);
    EXPECT_EQ(mixed.At(Size / 2 - 8, row), kRed) << "submesh 0, left: opaque";
    const Rgb glassPixel = mixed.At(Size / 2 + 8, row);
    const int expectedGreen = VulkanTesting::Blend::BlendChannel(255, 0, 0.5, false);
    EXPECT_EQ(glassPixel.r, 0);
    EXPECT_NEAR(glassPixel.g, expectedGreen, 1) << "submesh 1, right: 0.5 green over black, not the software kGreen";
    EXPECT_EQ(glassPixel.b, 0);
    EXPECT_EQ(mixed.Count(kGreen), 0) << "no pixel is the opaque green";
    EXPECT_EQ(mixedSoftware.At(Size / 2 + 8, row), kGreen) << "the software renderer draws it opaque";
    const int blendedPixels = [&mixed, expectedGreen]
    {
        int count = 0;
        for (int y = 0; y < Size; ++y)
        {
            for (int x = 0; x < Size; ++x)
            {
                const Rgb p = mixed.At(x, y);
                count += (p.r == 0 && p.b == 0 && p.g >= expectedGreen - 1 && p.g <= expectedGreen + 1) ? 1 : 0;
            }
        }
        return count;
    }();
    EXPECT_NEAR(blendedPixels, mixed.Count(kRed), mixed.Count(kRed) / 10) << "two halves of one size";

    // The opaque part against the software frame, strictly (the blended half is left out, with its seam)
    const std::vector<bool> blendedHalf = VulkanTesting::Blend::Dilate(
        VulkanTesting::Blend::CoveredPixels(
            [&]
            {
                // A flat render of the right half only: submesh 1 with a flat opaque material, on the software renderer
                return RenderOnSoftware([&mesh](Scene& scene)
                {
                    auto* renderer = AddMesh(scene, "RightHalf", mesh);
                    renderer->SetMaterial(0, Unlit(Common::Color::Black));
                    renderer->SetMaterial(1, Unlit(Common::Color::Green));
                }, camera).AsImage();
            }(),
            {0, 0, 0}),
        Size, Size, 2);
    VulkanTesting::Diff::Options options;
    options.ignore = &blendedHalf;
    EXPECT_TRUE(VulkanTesting::Diff::Agree(mixed.AsImage(), mixedSoftware.AsImage(), options));
    const std::string described = VulkanTesting::Diff::Compare(mixed.AsImage(), mixedSoftware.AsImage(), options).Describe();
    std::cout << "[ cross-diff ] mixed opaque and blended submeshes (opaque part, strict): " << described << std::endl;
    VulkanTesting::Diff::RecordedLines().push_back("mesh golden, mixed opaque and blended (opaque part): " + described);
}

TEST_F(WebGpuMeshGoldenTest, TheTransparentQueueReachesTheTargetBackToFrontWhateverTheHierarchyOrder)
{
    // Two half-alpha unlit quads (AlphaMode::Blend, so both are drawn by Scene::Render's Transparent queue with
    // RenderState::Transparent()) that overlap on screen, at different depths. The one added to the scene FIRST is the
    // NEAR one, so hierarchy order is the opposite of back to front: only Scene::Render's sort by depth draws the far one
    // first. Neither writes depth, so the order of the draws alone decides the overlap: far then near gives
    // a_near * near + (1 - a_near) * (a_far * far + (1 - a_far) * black), the other order a different colour.
    //
    // The reference is the GL-semantics composite of BlendReference.hpp, never a single software frame (the software
    // renderer cannot blend): each quad rendered OPAQUE and alone on the software renderer gives its colour and coverage,
    // and the two layers are blended on the CPU, far then near, over the software frame of the empty scene. The WebGPU
    // frame must match it strictly (2/255 per channel, every pixel: unlit and flat, as the other strict scenes), and must
    // NOT match the composite in the hierarchy order. Then the depths are swapped (same hierarchy order, same colours):
    // the other quad is drawn first now, and the frame follows.
    using VulkanTesting::Blend::Layer;
    const Camera camera = CameraAt(Math::Vector3(0.0f, 0.0f, 3.0f));
    // The near quad (z = 1, 2 units from the camera) spans about x 3..41 and y 13..51 on screen; the far one (z = 0,
    // 3 units) about x 26..51 and y 19..45: they overlap around x 26..41, y 19..45
    const Math::Vector3 nearLeft(-0.25f, 0.0f, 1.0f);
    const Math::Vector3 farRight(0.25f, 0.0f, 0.0f);
    const auto halfAlpha = [](const Common::Color& colour)
    {
        auto material = Unlit(Common::Color{colour.r, colour.g, colour.b, 0.5f});
        material->SetAlphaMode(AlphaMode::Blend);
        return material;
    };
    // Red at `redAt` added first, blue at `blueAt` added second
    const auto scene = [&halfAlpha](const Math::Vector3 redAt, const Math::Vector3 blueAt)
    {
        return [&halfAlpha, redAt, blueAt](Scene& s)
        {
            AddMesh(s, "Red", Mesh::GetBuiltin(BuiltinMesh::Quad), redAt)->SetMaterial(0, halfAlpha(Common::Color::Red));
            AddMesh(s, "Blue", Mesh::GetBuiltin(BuiltinMesh::Quad), blueAt)->SetMaterial(0, halfAlpha(Common::Color::Blue));
        };
    };
    // One quad drawn opaque and alone on the software renderer: a layer of the composite
    const auto layer = [&camera](const Common::Color& colour, const Math::Vector3 at)
    {
        const Frame flat = RenderOnSoftware([&colour, at](Scene& s)
        {
            AddMesh(s, "Layer", Mesh::GetBuiltin(BuiltinMesh::Quad), at)->SetMaterial(0, Unlit(colour));
        }, camera);
        Layer result;
        result.colour = flat.AsImage();
        result.covered = VulkanTesting::Blend::CoveredPixels(result.colour, {0, 0, 0});
        result.alpha = 0.5;
        return result;
    };
    const VulkanTesting::Diff::Image empty = RenderOnSoftware([](Scene&) {}, camera).AsImage();
    constexpr int overlapX = 34;
    constexpr int overlapY = 32;

    const auto check = [&](const Math::Vector3 redAt, const Math::Vector3 blueAt, const bool redIsFar, const std::string& what)
    {
        const Frame webgpu = RenderOn(R(), scene(redAt, blueAt), camera);
        const Layer red = layer(Common::Color::Red, redAt);
        const Layer blue = layer(Common::Color::Blue, blueAt);
        const VulkanTesting::Diff::Image backToFront =
            VulkanTesting::Blend::Composite(empty, redIsFar ? std::vector<Layer>{red, blue} : std::vector<Layer>{blue, red});
        const VulkanTesting::Diff::Image hierarchyOrder = VulkanTesting::Blend::Composite(empty, std::vector<Layer>{red, blue});
        const VulkanTesting::Diff::Image frontToBack =
            VulkanTesting::Blend::Composite(empty, redIsFar ? std::vector<Layer>{blue, red} : std::vector<Layer>{red, blue});

        // Both quads cover the overlap pixel (the scene means what it says)
        const std::size_t overlap = static_cast<std::size_t>(overlapY) * static_cast<std::size_t>(Size) +
            static_cast<std::size_t>(overlapX);
        EXPECT_TRUE(overlap < red.covered.size() && overlap < blue.covered.size() && red.covered[overlap] &&
                    blue.covered[overlap])
            << what;

        const VulkanTesting::Diff::Options strict{};
        EXPECT_TRUE(VulkanTesting::Diff::Agree(webgpu.AsImage(), backToFront, strict)) << what << ": back to front";
        EXPECT_FALSE(VulkanTesting::Diff::Agree(webgpu.AsImage(), frontToBack, strict))
            << what << ": not front to back (the order matters in this scene)";
        if (!redIsFar)
        {
            // The hierarchy order (red first) is front to back here
            EXPECT_FALSE(VulkanTesting::Diff::Agree(webgpu.AsImage(), hierarchyOrder, strict)) << what << ": not hierarchy order";
        }
        const std::string described = VulkanTesting::Diff::Compare(webgpu.AsImage(), backToFront, strict).Describe();
        std::cout << "[ cross-diff ] transparent queue back to front, " << what << " (strict, GL composite): " << described
                  << std::endl;
        VulkanTesting::Diff::RecordedLines().push_back("mesh golden, transparent queue back to front, " + what + ": " +
                                                       described);

        // The overlap's colour, from GL's equation: the far quad over black, then the near quad over that
        const Rgb nearColour = redIsFar ? kBlue : kRed;
        const Rgb farColour = redIsFar ? kRed : kBlue;
        const auto over = [](const Rgb source, const Rgb destination)
        {
            return Rgb{VulkanTesting::Blend::BlendChannel(source.r, destination.r, 0.5, false),
                       VulkanTesting::Blend::BlendChannel(source.g, destination.g, 0.5, false),
                       VulkanTesting::Blend::BlendChannel(source.b, destination.b, 0.5, false)};
        };
        const Rgb expected = over(nearColour, over(farColour, kBlack));
        const Rgb got = webgpu.At(overlapX, overlapY);
        EXPECT_NEAR(got.r, expected.r, 2) << what;
        EXPECT_NEAR(got.g, expected.g, 2) << what;
        EXPECT_NEAR(got.b, expected.b, 2) << what;
        return got;
    };

    // Red near (added first), blue far: blue is drawn first, red over it: about (128, 0, 64) in the overlap
    const Rgb redNear = check(nearLeft, farRight, false, "red near, blue far");
    EXPECT_GT(redNear.r, redNear.b + 40) << "the near red is on top";
    // The depths swapped, the hierarchy order kept: red is drawn first now, blue over it: about (64, 0, 128)
    const Rgb redFar = check(farRight, nearLeft, true, "red far, blue near");
    EXPECT_GT(redFar.b, redFar.r + 40) << "the near blue is on top";
}
