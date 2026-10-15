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
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Texture.hpp"

#include "CrossBackendDiff.hpp"
#include "VulkanTestFixture.hpp"

// Golden-image tests of MeshRenderer on the Vulkan backend (#43, stages B and C): the unlit scenes and the lit one of
// tests/engine/rendering/SoftwareMeshGoldenTests.cpp, with the same scenes, cameras, sizes and structural expectations
// (colours in screen regions, never exact images): scenes drawn by Scene::Render on a Vulkan device (Mesa lavapipe in CI;
// no window, the offscreen target) and read back. A textured cube must show its texture upright on the faces the camera
// sees (so a flipped UV, a swapped axis or a wrong winding fails), a cutout material must leave holes, a double-sided quad
// must show from behind and a single-sided one must not, and two submeshes must show their two materials, and (stage C) a
// lit face toward the light must be brighter than one edge-on to it. The blended "mixed" part of the two-submesh test is
// stage D's.
//
// Every scene is also rendered on a headless SoftwareRenderer and the two frames are compared pixel by pixel (the cross
// backend diff, CrossBackendDiff.hpp): the unlit scenes are nearest-sampled, so the software renderer is exact and the
// proposal's strict threshold applies, |Vulkan - Software| <= 2/255 per channel; the lit scene uses the lit criterion (mean
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
using Renderer::Vulkan::VulkanRenderer;
using VulkanTesting::VulkanTest;

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
            auto scene = Scene::Create("VulkanMeshGolden");
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

    /// The fixture: a Vulkan renderer with validation on at Size x Size, and the compiled shaders required. Renders a scene
    /// on it and on the software renderer and checks the two against each other.
    class VulkanMeshGoldenTest : public VulkanTest
    {
    protected:
        void SetUp() override
        {
            VulkanTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            N2_REQUIRE_BUILT_IN_SHADERS();
            ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));
        }

        /// The scene on Vulkan, with the cross-backend comparison against the software renderer. `options` are the diff's
        /// (the default: the proposal's strict 2/255 per channel, every pixel)
        Frame Render(const std::function<void(Scene&)>& build, const Camera& camera, const std::string& what,
                     VulkanTesting::Diff::Options options = {},
                     const Renderer::Common::SceneLightingData* lighting = nullptr,
                     const std::function<void(Scene&)>& maskBuild = nullptr)
        {
            const Frame vulkan = RenderOn(R(), build, camera, lighting);
            const Frame software = RenderOnSoftware(build, camera, lighting);
            // A lit comparison takes its coverage edges from the same geometry drawn flat unlit (`maskBuild`) on each
            // backend, never from the lit pixels
            Frame vulkanMask;
            Frame softwareMask;
            VulkanTesting::Diff::Image maskA;
            VulkanTesting::Diff::Image maskB;
            if (maskBuild)
            {
                vulkanMask = RenderOn(R(), maskBuild, camera, nullptr);
                softwareMask = RenderOnSoftware(maskBuild, camera, nullptr);
                maskA = vulkanMask.AsImage();
                maskB = softwareMask.AsImage();
                options.coverageA = &maskA;
                options.coverageB = &maskB;
            }
            else
            {
                EXPECT_EQ(options.maxMeanError, 0.0) << what << ": a lit comparison needs a coverage mask scene";
            }
            EXPECT_TRUE(VulkanTesting::Diff::Agree(vulkan.AsImage(), software.AsImage(), options)) << what;
            // The measured numbers, for the log (the PR body quotes them)
            const std::string described = VulkanTesting::Diff::Compare(vulkan.AsImage(), software.AsImage(), options).Describe();
            std::cout << "[ cross-diff ] " << what << ": " << described << std::endl;
            VulkanTesting::Diff::RecordedLines().push_back("mesh golden, " + what + ": " + described);
            return vulkan;
        }
    };
}

TEST_F(VulkanMeshGoldenTest, ATexturedCubeShowsItsCheckerUprightOnTheFacesTheCameraSees)
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

TEST_F(VulkanMeshGoldenTest, ALitFaceTowardTheLightIsBrighterThanOneAwayFromIt)
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

TEST_F(VulkanMeshGoldenTest, AnAlphaMaskMaterialLeavesHoles)
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

TEST_F(VulkanMeshGoldenTest, ADoubleSidedQuadShowsFromBehindAndASingleSidedOneDoesNot)
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

TEST_F(VulkanMeshGoldenTest, TwoSubmeshesShowTheirTwoMaterials)
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

    // The software test also draws a mixed opaque and blended mesh here: that part waits for blending (stage D of #43)
}
