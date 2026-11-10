#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The screen tint (RenderState::tint, IRenderer::SetScreenTint; effects design, stage E2) on the Vulkan backend: the
// fragment shaders test gl_FragCoord against the circle, which the backend places in rows counted down from the top (the
// flipped viewport). Real frames on a Vulkan device (Mesa lavapipe in CI), read back with row 0 at the bottom, so a circle
// placed upside down shows. The blended cases check the linearity the design rests on with real GPU blending: tinting each
// draw and blending gives the frame tinted after blending. The software renderer is the reference
// (SoftwareScreenTintTests.cpp).

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::ScreenTint;

namespace
{
    /// The 8-bit colour a tint of `saturation` makes of (r, g, b) (0..1 floats), as the shaders compute it
    Rgb Tinted(float r, float g, float b, const float saturation)
    {
        ScreenTint::Saturated(r, g, b, saturation);
        const auto to8 = [](const float x) { return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
        return {to8(r), to8(g), to8(b)};
    }

    ::testing::AssertionResult Near(const Rgb& actual, const Rgb& expected, const int tolerance = 2)
    {
        if (std::abs(actual.r - expected.r) > tolerance || std::abs(actual.g - expected.g) > tolerance ||
            std::abs(actual.b - expected.b) > tolerance)
        {
            return ::testing::AssertionFailure() << "(" << int(actual.r) << "," << int(actual.g) << "," << int(actual.b)
                                                 << ") is not (" << int(expected.r) << "," << int(expected.g) << ","
                                                 << int(expected.b) << ") within " << tolerance;
        }
        return ::testing::AssertionSuccess();
    }

    ScreenTint Circle(const float saturation, const float x, const float y, const float radius, const bool inside = true)
    {
        ScreenTint tint;
        tint.saturation = saturation;
        tint.centreX = x;
        tint.centreY = y;
        tint.radius = radius;
        tint.inside = inside;
        return tint;
    }

    class VulkanScreenTintTest : public VulkanDrawTest
    {
    protected:
        IMesh* Full(const float z) { return MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, z)); }
    };
}

TEST_F(VulkanScreenTintTest, NoTintLeavesTheFrameByteIdentical)
{
    IMesh* quad = Full(0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    const Frame plain = RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    const Frame cleared = RenderFrame([&] {
        R().SetScreenTint(ScreenTint{});
        R().DrawMesh(quad, Identity, red, RenderState::Opaque());
    });
    EXPECT_EQ(cleared.rgba, plain.rgba);
    EXPECT_TRUE(R().SupportsScreenTint());
}

TEST_F(VulkanScreenTintTest, AnOffCentreCircleIsWhereItsCentreSaysCountedFromTheBottomLeft)
{
    IMesh* quad = Full(0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    // Centre (0.25, 0.75): pixel (16, 48) counted up from the bottom; radius 0.25 half heights = 8 pixels
    const Frame frame = RenderFrame([&] {
        R().SetScreenTint(Circle(0.0f, 0.25f, 0.75f, 0.25f));
        R().DrawMesh(quad, Identity, red, RenderState::Opaque());
        R().SetScreenTint(ScreenTint{});
    });
    EXPECT_TRUE(Near(frame.At(16, 48), Tinted(1.0f, 0.0f, 0.0f, 0.0f)));
    EXPECT_EQ(frame.At(16, 16), kRed) << "not mirrored top to bottom";
    EXPECT_EQ(frame.At(48, 48), kRed) << "not mirrored left to right";

    // inside = false: the reverse
    const Frame outside = RenderFrame([&] {
        R().SetScreenTint(Circle(0.0f, 0.25f, 0.75f, 0.25f, false));
        R().DrawMesh(quad, Identity, red, RenderState::Opaque());
        R().SetScreenTint(ScreenTint{});
    });
    EXPECT_EQ(outside.At(16, 48), kRed);
    EXPECT_TRUE(Near(outside.At(16, 16), Tinted(1.0f, 0.0f, 0.0f, 0.0f)));
}

TEST_F(VulkanScreenTintTest, ABlendedDrawOverAnotherIsTheBlendedFrameTinted)
{
    IMesh* back = Full(0.5f);
    IMesh* front = Full(0.0f);
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMaterial* halfGreen = Colour(0.0f, 1.0f, 0.0f, 0.5f);
    constexpr float saturation = 0.2f;

    // Normal blending: the blended frame is (0.5, 0.5, 0) before the tint
    const Frame normal = RenderFrame([&] {
        R().SetScreenTint(Circle(saturation, 0.5f, 0.5f, ScreenTint::WholeScreenRadius));
        R().DrawMesh(back, Identity, red, RenderState::Opaque());
        R().DrawMesh(front, Identity, halfGreen, RenderState::Transparent());
        R().SetScreenTint(ScreenTint{});
    });
    EXPECT_TRUE(Near(normal.At(32, 32), Tinted(0.5f, 0.5f, 0.0f, saturation)));

    // Additive: (1, 0.5, 0) before the tint
    RenderState additive = RenderState::Transparent();
    additive.additive = true;
    const Frame added = RenderFrame([&] {
        R().SetScreenTint(Circle(saturation, 0.5f, 0.5f, ScreenTint::WholeScreenRadius));
        R().DrawMesh(back, Identity, red, RenderState::Opaque());
        R().DrawMesh(front, Identity, halfGreen, additive);
        R().SetScreenTint(ScreenTint{});
    });
    EXPECT_TRUE(Near(added.At(32, 32), Tinted(1.0f, 0.5f, 0.0f, saturation)));
}
