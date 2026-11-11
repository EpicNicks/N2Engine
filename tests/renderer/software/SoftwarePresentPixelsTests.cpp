#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <renderer/common/Surface.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// The software renderer's present per platform (build targets stage 7; docs/platform.html#surfaces): on a surface that
// takes the frame from the CPU (presentPixels, as Android's window buffer does) it needs no OpenGL context, and each
// Present hands over the finished frame in ReadFramebuffer's layout.
using Renderer::Common::Surface;
using Renderer::Software::SoftwareRenderer;

TEST(SoftwarePresentPixelsTest, ASurfaceThatTakesPixelsGetsEachFrameWithoutOpenGl)
{
    int presents = 0;
    std::vector<std::uint8_t> shown;
    int shownWidth = 0, shownHeight = 0;
    Surface surface;
    surface.kind = Surface::Kind::AndroidWindow;
    surface.handle = &presents; // any non-null native handle: nothing dereferences it here
    surface.presentPixels = [&](const std::uint8_t *rgba, const int width, const int height) {
        ++presents;
        shownWidth = width;
        shownHeight = height;
        shown.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    };
    // No makeContextCurrent or getProcAddress: a GL path would refuse to start
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(surface, 8, 4));

    renderer.Clear(1.0f, 0.0f, 0.0f, 1.0f);
    renderer.BeginFrame();
    renderer.EndFrame();
    renderer.Present();
    ASSERT_EQ(presents, 1);
    EXPECT_EQ(shownWidth, 8);
    EXPECT_EQ(shownHeight, 4);
    ASSERT_EQ(shown.size(), 8u * 4u * 4u);

    std::vector<std::uint8_t> read(8 * 4 * 4);
    renderer.ReadFramebuffer(read.data(), 8, 4);
    EXPECT_EQ(shown, read) << "the frame shown is the frame ReadFramebuffer returns (rows bottom to top)";
    renderer.Shutdown();
}

TEST(SoftwarePresentPixelsTest, AWindowWithNeitherAContextNorPixelsIsRefused)
{
    Surface surface;
    surface.kind = Surface::Kind::DesktopWindow;
    int dummy = 0;
    surface.handle = &dummy;
    SoftwareRenderer renderer;
    EXPECT_FALSE(renderer.Initialize(surface, 8, 4)) << "no way to present: refused rather than drawing nowhere";
}
