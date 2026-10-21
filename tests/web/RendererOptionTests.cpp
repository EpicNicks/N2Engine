#include <gtest/gtest.h>

#include <string>

#include "WebRendererOption.hpp"

// The web player's --renderer argument (web/player/WebRendererOption.*, stage G4 of #155)

using namespace N2Engine;
using RenderBackend = Config::ApplicationOptions::RenderBackend;

TEST(WebRendererOption, SoftwareAndWebGpuAreTheTwoBackends)
{
    const auto software = Web::ParseWebRenderer("software");
    ASSERT_TRUE(software.has_value()) << software.error();
    EXPECT_EQ(*software, RenderBackend::SOFTWARE);

    const auto webgpu = Web::ParseWebRenderer("webgpu");
    ASSERT_TRUE(webgpu.has_value()) << webgpu.error();
    EXPECT_EQ(*webgpu, RenderBackend::WEBGPU);
}

TEST(WebRendererOption, NativeBackendsAreRefusedByName)
{
    for (const char *name : {"opengl", "vulkan"})
    {
        const auto backend = Web::ParseWebRenderer(name);
        ASSERT_FALSE(backend.has_value()) << name;
        EXPECT_EQ(backend.error(), "Invalid renderer: " + std::string(name) +
                                       " is a native renderer, not in the web player (expected software or webgpu)");
    }
}

TEST(WebRendererOption, AnythingElseIsAnInvalidRenderer)
{
    for (const char *name : {"", "WebGPU", "Software", "webgl", "webgpu ", "gpu"})
    {
        const auto backend = Web::ParseWebRenderer(name);
        ASSERT_FALSE(backend.has_value()) << "'" << name << "'";
        EXPECT_EQ(backend.error(), "Invalid renderer: " + std::string(name) + " (expected software or webgpu)");
    }
}
