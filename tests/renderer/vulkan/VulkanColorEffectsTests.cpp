#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include <renderer/common/ColorEffects.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The unlit shader's colour effects (uSaturation, uFlash; Common::ColorEffects, effects design stage E1) on the Vulkan backend:
// real frames on a Vulkan device (Mesa lavapipe in CI). The values travel in the material block, so these also check the
// block's layout end to end (uFlash at offset 32). The software renderer is the reference (SoftwareColorEffectsTests.cpp).

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::ColorEffects;

namespace
{
    Rgb Expected(float r, float g, float b, const ColorEffects& effects)
    {
        effects.Apply(r, g, b);
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

    ColorEffects Effects(const float saturation, const std::array<float, 4>& flash = {0, 0, 0, 0})
    {
        ColorEffects effects;
        effects.saturation = saturation;
        effects.flash = flash;
        return effects;
    }

    void Set(IMaterial* material, const ColorEffects& effects)
    {
        material->SetFloat("uSaturation", effects.saturation);
        material->SetVec4("uFlash", effects.flash[0], effects.flash[1], effects.flash[2], effects.flash[3]);
    }

    class VulkanColorEffectsTest : public VulkanDrawTest
    {
    protected:
        IMesh* Full() { return MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f)); }
    };
}

TEST_F(VulkanColorEffectsTest, ANewMaterialHasNoEffects)
{
    IMesh* quad = Full();
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    const Frame plain = RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    EXPECT_EQ(plain.At(32, 32), kRed) << "uSaturation starts at 1 (the table's default), not 0";

    IMaterial* explicitDefaults = Colour(1.0f, 0.0f, 0.0f);
    Set(explicitDefaults, ColorEffects{});
    const Frame same = RenderFrame([&] { R().DrawMesh(quad, Identity, explicitDefaults, RenderState::Opaque()); });
    EXPECT_EQ(same.rgba, plain.rgba);
}

TEST_F(VulkanColorEffectsTest, SaturationZeroDrawsRedAsItsLumaGrey)
{
    IMesh* quad = Full();
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    Set(red, Effects(0.0f));
    const Frame frame = RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    EXPECT_TRUE(Near(frame.At(32, 32), Expected(1, 0, 0, Effects(0.0f))));
}

TEST_F(VulkanColorEffectsTest, AFlashMixesByItsAlphaAfterTheSaturation)
{
    IMesh* quad = Full();
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    const ColorEffects white = Effects(1.0f, {1, 1, 1, 0.5f});
    Set(red, white);
    const Frame flashed = RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    EXPECT_TRUE(Near(flashed.At(32, 32), Expected(1, 0, 0, white)));

    const ColorEffects greyThenBlue = Effects(0.0f, {0, 0, 1, 0.5f});
    Set(red, greyThenBlue);
    const Frame both = RenderFrame([&] { R().DrawMesh(quad, Identity, red, RenderState::Opaque()); });
    EXPECT_TRUE(Near(both.At(32, 32), Expected(1, 0, 0, greyThenBlue)));
}

TEST_F(VulkanColorEffectsTest, TwoMaterialsInOneFrameKeepTheirOwnValues)
{
    // Each draw's material block is its own dynamic-offset range: one sprite's flash never leaks into the next
    IMesh* left = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f));
    IMesh* right = MakeMesh(Quad(0.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* grey = Colour(0.0f, 1.0f, 0.0f);
    IMaterial* plain = Colour(0.0f, 1.0f, 0.0f);
    Set(grey, Effects(0.0f));
    const Frame frame = RenderFrame([&] {
        R().DrawMesh(left, Identity, grey, RenderState::Opaque());
        R().DrawMesh(right, Identity, plain, RenderState::Opaque());
    });
    EXPECT_TRUE(Near(frame.At(16, 32), Expected(0, 1, 0, Effects(0.0f))));
    EXPECT_EQ(frame.At(48, 32), kGreen);
}
