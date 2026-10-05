#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

// Texture options, sampling, UpdateMesh and the text shader query: the parts of the renderer interface that
// run without a window. The software renderer's resource calls never touch GL, so it is used uninitialized.
// Its pixels are tested headless in SoftwareRendererGoldenTests.cpp.

using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::Vertex;

namespace
{
    MeshData Triangle(const float x)
    {
        MeshData data;
        data.vertices = {
            Vertex{{x, 0, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{x + 1, 0, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{x, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2};
        return data;
    }

    MeshData Quad()
    {
        MeshData data = Triangle(5);
        data.vertices.push_back(Vertex{{6, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}});
        data.indices = {0, 1, 3, 0, 3, 2};
        return data;
    }

    // Overrides only what IRenderer requires, so the text additions use their default bodies
    class MinimalRenderer final : public Renderer::Common::IRenderer
    {
    public:
        int plainCreateTextureCalls = 0;

        ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override
        {
            ++plainCreateTextureCalls;
            return nullptr;
        }

        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return false; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return false; }
        IMesh *CreateMesh(const MeshData &) override { return nullptr; }
        void DestroyMesh(IMesh *) override {}
        void DestroyTexture(ITexture *) override {}
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *) override { return nullptr; }
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *, ITexture *) override
        {
            return nullptr;
        }
        void DestroyMaterial(Renderer::Common::IMaterial *) override {}
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &,
                                 const N2Engine::Math::Vector3 &) override {}
        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *, const float *, Renderer::Common::IMaterial *,
                      const Renderer::Common::RenderState &) override {}
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] Renderer::Common::IShader *GetStandardUnlitShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Minimal"; }
    };
}

// ============================================================================
// TextureOptions
// ============================================================================

TEST(TextureOptionsTest, DefaultsAreWhatTexturesHadBeforeOptions)
{
    constexpr TextureOptions options{};
    EXPECT_EQ(options.filter, TextureFilter::Linear);
    EXPECT_EQ(options.wrap, TextureWrap::Repeat);
    EXPECT_TRUE(options.mipmaps);
    EXPECT_EQ(TextureOptions::Default(), options);
}

TEST(TextureOptionsTest, SdfAtlasIsLinearClampedWithoutMipmaps)
{
    constexpr TextureOptions options = TextureOptions::SdfAtlas();
    EXPECT_EQ(options.filter, TextureFilter::Linear);
    EXPECT_EQ(options.wrap, TextureWrap::ClampToEdge);
    EXPECT_FALSE(options.mipmaps);
    EXPECT_NE(options, TextureOptions::Default());
}

TEST(TextureOptionsTest, TheDefaultBodyForwardsToThePlainOverload)
{
    MinimalRenderer renderer;
    Renderer::Common::IRenderer &base = renderer;
    const std::uint8_t pixel = 128;

    EXPECT_EQ(base.CreateTexture(&pixel, 1, 1, 1, TextureOptions::SdfAtlas()), nullptr);
    EXPECT_EQ(renderer.plainCreateTextureCalls, 1);
}

TEST(TextureOptionsTest, SoftwareTexturesKeepTheirOptionsAndSingleChannelData)
{
    Renderer::Software::SoftwareRenderer renderer;
    const std::vector<std::uint8_t> pixels{0, 64, 128, 255, 10, 20};

    ITexture *atlas = renderer.CreateTexture(pixels.data(), 3, 2, 1, TextureOptions::SdfAtlas());
    ASSERT_NE(atlas, nullptr);
    const auto *sw = dynamic_cast<Renderer::Software::SWTexture *>(atlas);
    ASSERT_NE(sw, nullptr);
    EXPECT_EQ(sw->options, TextureOptions::SdfAtlas());
    EXPECT_EQ(sw->GetWidth(), 3u);
    EXPECT_EQ(sw->GetHeight(), 2u);
    EXPECT_EQ(sw->GetChannels(), 1u);
    EXPECT_EQ(sw->data, pixels);

    ITexture *plain = renderer.CreateTexture(pixels.data(), 3, 2, 1);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(dynamic_cast<Renderer::Software::SWTexture *>(plain)->options, TextureOptions::Default());

    EXPECT_EQ(renderer.CreateTexture(nullptr, 3, 2, 1, TextureOptions::SdfAtlas()), nullptr);
    EXPECT_EQ(renderer.CreateTexture(pixels.data(), 0, 2, 1, TextureOptions::SdfAtlas()), nullptr);

    renderer.DestroyTexture(atlas);
    renderer.DestroyTexture(plain);
}

TEST(TextureOptionsTest, SoftwareSamplingHonoursWrap)
{
    Renderer::Software::SWTexture texture;
    texture.width = 2;
    texture.height = 1;
    texture.channels = 1;
    texture.data = {0, 255};

    // u = 1.25 repeats to 0.25 (the first texel); clamped it is 1 (the last)
    texture.options.wrap = TextureWrap::Repeat;
    EXPECT_EQ(texture.Sample(1.25f, 0.0f) & 0xFFu, 0u);
    texture.options.wrap = TextureWrap::ClampToEdge;
    EXPECT_EQ(texture.Sample(1.25f, 0.0f) & 0xFFu, 255u);
    EXPECT_EQ(texture.Sample(-0.5f, 0.0f) & 0xFFu, 0u);
}

TEST(TextureOptionsTest, SoftwareSamplingFillsMissingChannelsAsOpenGLDoes)
{
    // OpenGL reads R8 as (r, 0, 0, 1) and RG8 as (r, g, 0, 1); the software renderer matches it
    Renderer::Software::SWTexture red;
    red.width = 1;
    red.height = 1;
    red.channels = 1;
    red.data = {200};
    EXPECT_EQ(red.Sample(0.5f, 0.5f), 0xFF0000C8u) << "(200, 0, 0, 255), r in the low byte";

    Renderer::Software::SWTexture redGreen;
    redGreen.width = 1;
    redGreen.height = 1;
    redGreen.channels = 2;
    redGreen.data = {200, 100};
    EXPECT_EQ(redGreen.Sample(0.5f, 0.5f), 0xFF0064C8u) << "(200, 100, 0, 255)";

    Renderer::Software::SWTexture rgb;
    rgb.width = 1;
    rgb.height = 1;
    rgb.channels = 3;
    rgb.data = {1, 2, 3};
    EXPECT_EQ(rgb.Sample(0.5f, 0.5f), 0xFF030201u) << "(1, 2, 3, 255)";
}

TEST(TextureOptionsTest, SoftwareBilinearSamplingMatchesGLLinear)
{
    // Two texels, 0 and 255, with centres at u = 0.25 and 0.75
    Renderer::Software::SWTexture texture;
    texture.width = 2;
    texture.height = 1;
    texture.channels = 1;
    texture.data = {0, 255};

    texture.options.wrap = TextureWrap::ClampToEdge;
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(0.25f, 0.5f), 0.0f, 1e-5f) << "on the first centre";
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(0.75f, 0.5f), 1.0f, 1e-5f) << "on the second centre";
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(0.5f, 0.5f), 0.5f, 1e-5f) << "halfway between";
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(0.0f, 0.5f), 0.0f, 1e-5f) << "clamped at the left edge";
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(1.0f, 0.5f), 1.0f, 1e-5f) << "clamped at the right edge";
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(-3.0f, 0.5f), 0.0f, 1e-5f) << "outside, clamped";

    // Repeating, the left edge is halfway between the last texel and the first
    texture.options.wrap = TextureWrap::Repeat;
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(0.0f, 0.5f), 0.5f, 1e-5f);
    EXPECT_NEAR(texture.SampleFirstChannelBilinear(1.25f, 0.5f), 0.0f, 1e-5f) << "1.25 repeats to 0.25";

    // Only the first channel is read
    Renderer::Software::SWTexture rg;
    rg.width = 1;
    rg.height = 1;
    rg.channels = 2;
    rg.data = {51, 255};
    EXPECT_NEAR(rg.SampleFirstChannelBilinear(0.3f, 0.7f), 0.2f, 1e-5f);
}

// ============================================================================
// UpdateMesh
// ============================================================================

TEST(UpdateMeshTest, TheDefaultBodyUpdatesNothing)
{
    MinimalRenderer renderer;
    Renderer::Common::IRenderer &base = renderer;
    Renderer::Software::SWMesh mesh;
    EXPECT_FALSE(base.UpdateMesh(&mesh, Triangle(0)));
    EXPECT_TRUE(mesh.vertices.empty());
}

TEST(UpdateMeshTest, SoftwareReplacesTheDataInPlace)
{
    Renderer::Software::SoftwareRenderer renderer;
    IMesh *mesh = renderer.CreateMesh(Triangle(0));
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_EQ(mesh->GetIndexCount(), 3u);

    const MeshData quad = Quad();
    ASSERT_TRUE(renderer.UpdateMesh(mesh, quad));
    EXPECT_EQ(mesh->GetVertexCount(), 4u);
    EXPECT_EQ(mesh->GetIndexCount(), 6u);
    const auto *sw = dynamic_cast<Renderer::Software::SWMesh *>(mesh);
    ASSERT_NE(sw, nullptr);
    EXPECT_EQ(sw->indices, quad.indices);
    EXPECT_FLOAT_EQ(sw->vertices[3].position[0], 6.0f);

    // Smaller data shrinks it
    ASSERT_TRUE(renderer.UpdateMesh(mesh, Triangle(9)));
    EXPECT_EQ(mesh->GetVertexCount(), 3u);
    EXPECT_FLOAT_EQ(sw->vertices[0].position[0], 9.0f);

    renderer.DestroyMesh(mesh);
}

TEST(UpdateMeshTest, SoftwareRejectsBadArgumentsWithoutChangingAnything)
{
    Renderer::Software::SoftwareRenderer renderer;
    IMesh *mesh = renderer.CreateMesh(Triangle(0));
    ASSERT_NE(mesh, nullptr);

    EXPECT_FALSE(renderer.UpdateMesh(nullptr, Quad()));
    EXPECT_FALSE(renderer.UpdateMesh(mesh, MeshData{})) << "no vertices";
    EXPECT_EQ(mesh->GetVertexCount(), 3u);

    // A mesh this renderer doesn't own (or already destroyed)
    Renderer::Software::SWMesh foreign;
    EXPECT_FALSE(renderer.UpdateMesh(&foreign, Quad()));
    EXPECT_TRUE(foreign.vertices.empty());

    renderer.DestroyMesh(mesh);
}

// ============================================================================
// Text shader
// ============================================================================

TEST(TextShaderTest, OnlyBackendsThatDrawTextHaveOne)
{
    MinimalRenderer minimal;
    EXPECT_EQ(static_cast<Renderer::Common::IRenderer &>(minimal).GetStandardTextShader(), nullptr);

    // The software renderer's built-in shaders exist before Initialize
    Renderer::Software::SoftwareRenderer software;
    Renderer::Common::IShader *text = software.GetStandardTextShader();
    ASSERT_NE(text, nullptr);
    const auto *swText = dynamic_cast<Renderer::Software::SWShader *>(text);
    ASSERT_NE(swText, nullptr);
    EXPECT_EQ(swText->GetType(), Renderer::Software::SWShaderType::Text);
    EXPECT_NE(text, software.GetStandardUnlitShader());
    EXPECT_NE(text, software.GetStandardLitShader());

    // A built-in shader isn't destroyed by DestroyShaderProgram
    EXPECT_TRUE(software.DestroyShaderProgram(text));
    EXPECT_EQ(software.GetStandardTextShader(), text);
}
