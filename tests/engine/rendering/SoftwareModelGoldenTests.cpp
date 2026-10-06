#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Model.hpp"

#include "GltfTestBuilder.hpp"

// A golden-image test of an instantiated model: a glTF built in the test (a quad split into two unlit materials,
// red and green, used by a node and by a mirrored node), instantiated and drawn by Scene::Render on a headless
// SoftwareRenderer. The model must cover its projected bounds, show both material colours, and keep the mirrored
// node visible (its triangles wind the other way on screen, so MeshRenderer culls front faces for it). The checks
// are structural (colours in screen regions), never exact images.

using namespace N2Engine;
using Rendering::Model;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 64;

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    constexpr Rgb kBlack{0, 0, 0};
    constexpr Rgb kRed{255, 0, 0};
    constexpr Rgb kGreen{0, 255, 0};

    struct Frame
    {
        std::vector<std::uint8_t> rgba;

        /// y counts up from the bottom row
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        /// Pixels in [x0, x1] x [y0, y1] that aren't the background
        [[nodiscard]] int Covered(const int x0, const int y0, const int x1, const int y1) const
        {
            int count = 0;
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    count += At(x, y) == kBlack ? 0 : 1;
                }
            }
            return count;
        }

        [[nodiscard]] int Count(const Rgb colour) const
        {
            int count = 0;
            for (int y = 0; y < Size; ++y)
            {
                for (int x = 0; x < Size; ++x)
                {
                    count += At(x, y) == colour ? 1 : 0;
                }
            }
            return count;
        }
    };

    /**
     * The model: mesh "Halves" (the left half of a unit quad facing +Z with material "Red", the right half with
     * "Green"), drawn by node "Plain" at x = -0.6 and node "Mirrored" at x = 0.6 with scale (-1, 1, 1)
     */
    std::shared_ptr<Model> MirroredHalvesModel()
    {
        GltfTest::Builder b = GltfTest::TwoMaterialQuad();
        b.doc["nodes"][0]["name"] = "Plain";
        b.doc["nodes"][0]["translation"] = {-0.6, 0.0, 0.0};
        const int mirrored = b.AddNode("Mirrored", {{"mesh", 0}, {"translation", {0.6, 0.0, 0.0}}, {"scale", {-1, 1, 1}}});
        b.SetScene({0, mirrored});
        const std::vector<std::uint8_t> glb = b.ToGlb();
        return Model::LoadFromMemory(glb, {}, {}, "Halves");
    }

    Frame RenderModel(const Model &model)
    {
        Camera camera;
        camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(Math::Vector3(0.0f, 0.0f, 3.0f));
        camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));

        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
        {
            auto scene = Scene::Create("SoftwareModelGolden");
            scene->AddRootGameObject(model.Instantiate());
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset(); // the scene and its components go before the renderer shuts down
        }
        renderer.Shutdown();
        return frame;
    }
}

TEST(SoftwareModelGoldenTest, AnInstantiatedModelCoversItsBoundsShowsBothMaterialsAndKeepsItsMirroredNode)
{
    const auto model = MirroredHalvesModel();
    ASSERT_NE(model, nullptr);
    const Frame frame = RenderModel(*model);

    // From (0, 0, 3) with a 45-degree field of view, a unit at z = 0 is about 25.75 pixels: the plain quad spans
    // x = -1.1 to -0.1 (pixels 4 to 29), the mirrored one 0.1 to 1.1 (pixels 35 to 60), both y = -0.5 to 0.5
    // (pixels 19 to 44). Inside each, two pixels in from every edge, everything is drawn.
    constexpr int y0 = 21;
    constexpr int y1 = 42;
    const int plainArea = (27 - 6 + 1) * (y1 - y0 + 1);
    EXPECT_EQ(frame.Covered(6, y0, 27, y1), plainArea) << "the plain node covers its projected bounds";
    const int mirroredArea = (58 - 37 + 1) * (y1 - y0 + 1);
    EXPECT_EQ(frame.Covered(37, y0, 58, y1), mirroredArea) << "the mirrored node is drawn, not culled";

    // Both materials, on the side each belongs: the plain node red on its left, the mirror red on its right
    EXPECT_EQ(frame.At(10, 32), kRed) << "plain, left half";
    EXPECT_EQ(frame.At(23, 32), kGreen) << "plain, right half";
    EXPECT_EQ(frame.At(41, 32), kGreen) << "mirrored: its green half is now on the left";
    EXPECT_EQ(frame.At(54, 32), kRed) << "mirrored: its red half is now on the right";
    EXPECT_GT(frame.Count(kRed), 400);
    EXPECT_GT(frame.Count(kGreen), 400);

    // Background between the two nodes, above and below them
    EXPECT_EQ(frame.At(32, 32), kBlack);
    EXPECT_EQ(frame.At(16, 55), kBlack);
    EXPECT_EQ(frame.At(48, 8), kBlack);
}
