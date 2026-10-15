#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanTestFixture.hpp"

// Helpers the Vulkan draw tests share (VulkanUnlitDrawTests.cpp, VulkanPipelineTests.cpp): a draw fixture that makes the
// fixture's renderer a 64x64 target, a frame read back from it (row 0 at the bottom, as ReadFramebuffer gives it) with
// colour and coverage queries, and the quads the software renderer's golden tests draw with (the draw tests mirror them).
// Every draw is made with identity view and projection unless a test says otherwise, so a vertex position is its NDC.

namespace VulkanTesting::Draw
{
    using Renderer::Common::IMaterial;
    using Renderer::Common::IMesh;
    using Renderer::Common::ITexture;
    using Renderer::Common::MeshData;
    using Renderer::Common::RenderState;
    using Renderer::Common::Vertex;
    using Renderer::Vulkan::VulkanRenderer;

    inline constexpr int Size = 64; // square frames

    inline constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb&) const = default;
    };

    inline constexpr Rgb kBlack{0, 0, 0};
    inline constexpr Rgb kRed{255, 0, 0};
    inline constexpr Rgb kGreen{0, 255, 0};
    inline constexpr Rgb kBlue{0, 0, 255};
    inline constexpr Rgb kYellow{255, 255, 0};
    inline constexpr Rgb kWhite{255, 255, 255};

    inline ::testing::AssertionResult SameColour(const Rgb& a, const Rgb& b)
    {
        if (a == b)
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "(" << int(a.r) << "," << int(a.g) << "," << int(a.b) << ") is not ("
                                             << int(b.r) << "," << int(b.g) << "," << int(b.b) << ")";
    }

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row, like NDC y
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] std::uint8_t AlphaAt(const int x, const int y) const
        {
            return rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 + 3];
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

        friend bool operator==(const Frame&, const Frame&) = default;
    };

    struct Coverage
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    /// The pixels of one colour: how many, and their bounding box
    inline Coverage CoverageOf(const Frame& frame, const Rgb colour)
    {
        Coverage c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (frame.At(x, y) == colour)
                {
                    ++c.count;
                    c.minX = std::min(c.minX, x);
                    c.maxX = std::max(c.maxX, x);
                    c.minY = std::min(c.minY, y);
                    c.maxY = std::max(c.maxY, y);
                }
            }
        }
        return c;
    }

    /// An axis-aligned quad in NDC (the tests draw with identity matrices) at depth z, wound counter-clockwise, with uvs
    /// 0..1 over it and one vertex colour
    inline MeshData Quad(const float x0, const float y0, const float x1, const float y1, const float z, const float r = 1.0f,
                         const float g = 1.0f, const float b = 1.0f, const float a = 1.0f)
    {
        MeshData data;
        data.vertices = {
            Vertex{{x0, y0, z}, {0, 0, 1}, {0, 0}, {r, g, b, a}},
            Vertex{{x1, y0, z}, {0, 0, 1}, {1, 0}, {r, g, b, a}},
            Vertex{{x1, y1, z}, {0, 0, 1}, {1, 1}, {r, g, b, a}},
            Vertex{{x0, y1, z}, {0, 0, 1}, {0, 1}, {r, g, b, a}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// One mesh holding two quads: the left half of the screen (indices 0..5), then the right half (6..11)
    inline MeshData TwoHalves()
    {
        MeshData data = Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
        const MeshData right = Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f);
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.insert(data.vertices.end(), right.vertices.begin(), right.vertices.end());
        for (const std::uint32_t index : right.indices)
        {
            data.indices.push_back(base + index);
        }
        return data;
    }

    /// A row-major translation (the matrices are passed as the engine has them: the translation in the last column)
    inline void Translation(float (&m)[16], const float x, const float y, const float z)
    {
        const float values[16] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
        std::copy(std::begin(values), std::end(values), m);
    }

    /// The fixture of the draw tests: validation on, a 64x64 target, and the shaders required (skip, or fail under
    /// N2_REQUIRE_VULKAN=1, when the build has none)
    class VulkanDrawTest : public VulkanTest
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
            R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
            _unlit = R().GetStandardUnlitShader();
            ASSERT_NE(_unlit, nullptr);
        }

        /// An unlit material of one colour
        IMaterial* Colour(const float r, const float g, const float b, const float a = 1.0f)
        {
            IMaterial* material = R().CreateMaterial(_unlit);
            EXPECT_NE(material, nullptr);
            if (material != nullptr)
            {
                material->SetColor("uAlbedo", r, g, b, a);
            }
            return material;
        }

        IMesh* MakeMesh(const MeshData& data)
        {
            IMesh* mesh = R().CreateMesh(data);
            EXPECT_NE(mesh, nullptr);
            return mesh;
        }

        /// The frame as it is now, read back from the fixture's renderer
        [[nodiscard]] Frame ReadFrame()
        {
            return ReadFrameOf(R());
        }

        [[nodiscard]] static Frame ReadFrameOf(const VulkanRenderer& renderer)
        {
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4), Size, Size};
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            return frame;
        }

        /// One frame of draws with identity view and projection, read back
        template <typename Draws>
        Frame RenderFrame(const Draws& draws)
        {
            R().BeginFrame();
            R().SetViewProjection(Identity, Identity);
            draws();
            R().EndFrame();
            return ReadFrame();
        }

        Renderer::Common::IShader* _unlit = nullptr;
    };
}
