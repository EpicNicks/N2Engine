#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <vector>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 64;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<std::uint8_t, 4>;
    constexpr Pixel Black{0, 0, 0, 255}, Red{255, 0, 0, 255}, Green{0, 255, 0, 255};

    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        Pixel At(int x, int y) const
        {
            const auto i = (y * Size + x) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        }
        int Count(Pixel value) const
        {
            int count = 0;
            for (int y = 0; y < Size; ++y)
                for (int x = 0; x < Size; ++x)
                    count += At(x, y) == value;
            return count;
        }
    };

    MeshData Triangle(float center = 0, float halfWidth = 0.75f, float depth = 0)
    {
        MeshData data;
        data.vertices = {
            Vertex{{center - halfWidth, -0.75f, depth}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{center + halfWidth, -0.75f, depth}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{center, 0.75f, depth}, {0, 0, 1}, {0.5f, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2};
        return data;
    }

    class SoftwareWireframeTest : public ::testing::Test
    {
      protected:
        SoftwareRenderer renderer;
        IMaterial *red = nullptr, *green = nullptr;
        IMesh *triangle = nullptr;

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            renderer.Clear(0, 0, 0, 1);
            red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            green = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            triangle = renderer.CreateMesh(Triangle());
            ASSERT_NE(red, nullptr);
            ASSERT_NE(green, nullptr);
            ASSERT_NE(triangle, nullptr);
            red->SetColor("uAlbedo", 1, 0, 0, 1);
            green->SetColor("uAlbedo", 0, 1, 0, 1);
        }
        void TearDown() override
        {
            renderer.Shutdown();
        }

        template <class Draw> Frame Render(Draw draw)
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draw();
            renderer.EndFrame();
            // Main-thread changes after submission must never race the render thread or affect queued draws.
            renderer.SetWireframe(false);
            Frame frame{std::vector<std::uint8_t>(Size * Size * 4)};
            renderer.ReadFramebuffer(frame.pixels.data(), Size, Size);
            return frame;
        }
    };
} // namespace

TEST_F(SoftwareWireframeTest, DrawsEdgesLeavesInteriorClearAndTurningOffRestoresSolid)
{
    const auto solid = Render([&] { renderer.DrawMesh(triangle, Identity, red); });
    renderer.SetWireframe(true);
    const auto wire = Render([&] { renderer.DrawMesh(triangle, Identity, red); });
    EXPECT_EQ(solid.At(32, 24), Red);
    EXPECT_EQ(wire.At(32, 24), Black);
    EXPECT_EQ(wire.At(32, 8), Red);
    EXPECT_EQ(wire.At(16, 24), Red);
    EXPECT_EQ(wire.At(47, 24), Red);
    EXPECT_GT(wire.Count(Red), 90);
    EXPECT_LT(wire.Count(Red), solid.Count(Red) / 3);
    const auto restored = Render([&] { renderer.DrawMesh(triangle, Identity, red); });
    EXPECT_EQ(restored.pixels, solid.pixels);
}

TEST_F(SoftwareWireframeTest, WireframeIsCapturedAtEachDrawIncludingIndexedRanges)
{
    auto data = Triangle(-0.5f, 0.35f);
    auto right = Triangle(0.5f, 0.35f);
    data.vertices.insert(data.vertices.end(), right.vertices.begin(), right.vertices.end());
    data.indices.insert(data.indices.end(), {3, 4, 5});
    auto *mesh = renderer.CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    const auto frame = Render([&] {
        renderer.SetWireframe(true);
        renderer.DrawMesh(mesh, Identity, red, RenderState{}, {0, 3});
        renderer.SetWireframe(false);
        renderer.DrawMesh(mesh, Identity, red, RenderState{}, {3, 3});
        renderer.SetWireframe(true); // Neither the last toggle nor EndFrame determines earlier draws.
    });
    EXPECT_EQ(frame.At(16, 24), Black);
    EXPECT_EQ(frame.At(48, 24), Red);
    EXPECT_GT(frame.Count(Red), 300);
}

TEST_F(SoftwareWireframeTest, CullingAndBothWindingsKeepTheirMeaning)
{
    auto reverse = Triangle();
    std::swap(reverse.indices[1], reverse.indices[2]);
    auto *clockwise = renderer.CreateMesh(reverse);
    ASSERT_NE(clockwise, nullptr);
    auto draw = [&](IMesh *mesh, CullMode cull) {
        return Render([&] {
            renderer.SetWireframe(true);
            RenderState state;
            state.cull = cull;
            renderer.DrawMesh(mesh, Identity, red, state);
        });
    };
    const auto ccw = draw(triangle, CullMode::None), cw = draw(clockwise, CullMode::None);
    EXPECT_EQ(ccw.pixels, cw.pixels);
    EXPECT_GT(draw(triangle, CullMode::Back).Count(Red), 90);
    EXPECT_EQ(draw(triangle, CullMode::Front).Count(Red), 0);
    EXPECT_EQ(draw(clockwise, CullMode::Back).Count(Red), 0);
    EXPECT_GT(draw(clockwise, CullMode::Front).Count(Red), 90);
}

TEST_F(SoftwareWireframeTest, ClippingDrawsOnlyPolygonBoundaryWithoutFanDiagonals)
{
    for (const bool reverse : {false, true})
    {
        auto data = Triangle();
        data.vertices[2].position[2] = -2; // Near clipping turns the triangle into a quadrilateral.
        if (reverse)
            std::swap(data.indices[1], data.indices[2]);
        auto *mesh = renderer.CreateMesh(data);
        ASSERT_NE(mesh, nullptr);
        const auto frame = Render([&] {
            renderer.SetWireframe(true);
            RenderState state;
            state.cull = CullMode::None;
            renderer.DrawMesh(mesh, Identity, red, state);
        });
        EXPECT_GT(frame.Count(Red), 65);
        EXPECT_EQ(frame.At(32, 31), Red) << "The new near-plane polygon boundary is visible";
        for (int y = 14; y < 27; ++y)
            for (int x = 26; x < 39; ++x)
                EXPECT_EQ(frame.At(x, y), Black) << "No internal fan diagonal at " << x << ',' << y;
    }
}

TEST_F(SoftwareWireframeTest, NarrowClippedFanKeepsBoundaryBandAcrossNeighboringTriangles)
{
    auto data = Triangle();
    const float points[3][3]{{-0.5f, -0.75f, -2}, {0.75f, -0.75f, 0.5f}, {0.015625f, 0.75f, -0.96875f}};
    for (int i = 0; i < 3; ++i)
        std::copy_n(points[i], 3, data.vertices[i].position);
    for (const auto &order : {std::array<unsigned, 3>{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}})
    {
        data.indices.assign(order.begin(), order.end());
        auto *mesh = renderer.CreateMesh(data);
        ASSERT_NE(mesh, nullptr);
        const auto frame = Render([&] {
            renderer.SetWireframe(true);
            RenderState state;
            state.cull = CullMode::None;
            renderer.DrawMesh(mesh, Identity, red, state);
        });
        EXPECT_EQ(frame.At(32, 20), Red) << "Band crosses a narrow fan wedge, regardless of fan anchor";
        EXPECT_EQ(frame.At(33, 40), Black);
    }
}

TEST_F(SoftwareWireframeTest, AClippedVertexExactlyOnThePlaneDoesNotFillThePolygon)
{
    auto data = Triangle();
    data.vertices[0].position[2] = -1; // Clipping emits this vertex twice at the inside/outside transition.
    data.vertices[1].position[2] = -2;
    auto *mesh = renderer.CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    const auto frame = Render([&] {
        renderer.SetWireframe(true);
        renderer.DrawMesh(mesh, Identity, red);
    });
    EXPECT_GT(frame.Count(Red), 50);
    EXPECT_EQ(frame.At(24, 32), Black) << "A zero-length boundary edge must not cover every pixel";
}

TEST_F(SoftwareWireframeTest, InteriorsWriteNoDepthAndEdgesRespectDepthWriteState)
{
    auto *back = renderer.CreateMesh(Triangle(0, 0.75f, 0.5f));
    ASSERT_NE(back, nullptr);
    auto draw = [&](bool depthWrite) {
        return Render([&] {
            RenderState state;
            state.depthWrite = depthWrite;
            renderer.SetWireframe(true);
            renderer.DrawMesh(triangle, Identity, red, state);
            renderer.SetWireframe(false);
            renderer.DrawMesh(back, Identity, green);
        });
    };
    const auto normal = draw(true);
    EXPECT_EQ(normal.At(32, 8), Red);
    EXPECT_EQ(normal.At(32, 24), Green);
    const auto noWrite = draw(false);
    EXPECT_EQ(noWrite.At(32, 8), Green);
    EXPECT_EQ(noWrite.At(32, 24), Green);
}

TEST_F(SoftwareWireframeTest, TexturedWireframeRetainsColorAndAlphaCutoff)
{
    const std::uint8_t texel[]{0, 255, 0, 128};
    auto *texture = renderer.CreateTexture(texel, 1, 1, 4, TextureOptions{});
    auto *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader(), texture);
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(material, nullptr);
    material->SetFloat("uAlphaCutoff", 0.75f);
    auto draw = [&] {
        return Render([&] {
            renderer.SetWireframe(true);
            renderer.DrawMesh(triangle, Identity, material);
        });
    };
    EXPECT_EQ(draw().Count(Black), Size * Size);
    material->SetFloat("uAlphaCutoff", 0.25f);
    const auto frame = draw();
    EXPECT_EQ(frame.At(32, 8), (Pixel{0, 255, 0, 128}));
    EXPECT_EQ(frame.At(32, 24), Black);
}
