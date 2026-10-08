#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>

#include "engine/rendering/Material.hpp"
#include "engine/rendering/Texture.hpp"

// The OpenGL lit shader cannot be compiled here (CI has no GPU), so its source is checked as text against what the C++
// side sets and what the software renderer shares: every uniform Material::ApplyUniforms sets is declared in the
// GLSL, the sampler uniforms and texture units line up, the tangent attribute is at the location the mesh sets up, and
// the PBR constants are LightingMath.hpp's. This catches a renamed or forgotten uniform, which on a GPU is silent: an
// unknown uniform location is ignored, and the shader reads 0.

using namespace N2Engine;
using Rendering::Material;
using Rendering::ShadingModel;
using Rendering::Texture;

namespace
{
    std::string ReadFile(const std::string &relative)
    {
        std::ifstream file(std::string(N2_RENDERER_SOURCE_DIR) + "/" + relative, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    /// The text of a raw-string shader in OpenGLRenderer.cpp: `const char *<name> = R"(` ... `)";`
    std::string ShaderSource(const std::string &file, const std::string &name)
    {
        const std::string marker = "const char *" + name + " = R\"(";
        const std::size_t start = file.find(marker);
        if (start == std::string::npos)
        {
            return {};
        }
        const std::size_t from = start + marker.size();
        const std::size_t end = file.find(")\";", from);
        return end == std::string::npos ? std::string{} : file.substr(from, end - from);
    }

    /// Whether `source` declares `uniform <type> <name>;` (or an array of it)
    bool DeclaresUniform(const std::string &source, const std::string &name)
    {
        std::size_t at = 0;
        while ((at = source.find(name, at)) != std::string::npos)
        {
            const std::size_t after = at + name.size();
            const char next = after < source.size() ? source[after] : '\0';
            const bool wholeWord = next == ';' || next == '[' || next == ' ';
            const std::size_t lineStart = source.rfind('\n', at) == std::string::npos ? 0 : source.rfind('\n', at) + 1;
            const std::string line = source.substr(lineStart, at - lineStart);
            if (wholeWord && line.find("uniform") != std::string::npos)
            {
                return true;
            }
            at = after;
        }
        return false;
    }

    /// Stands in for a GPU material: remembers every uniform name it is given. Its textures are all present.
    class RecordingMaterial final : public Renderer::Common::IMaterial
    {
    public:
        struct FakeTexture final : Renderer::Common::ITexture
        {
            [[nodiscard]] bool IsValid() const override { return true; }
            [[nodiscard]] std::uint32_t GetWidth() const override { return 1; }
            [[nodiscard]] std::uint32_t GetHeight() const override { return 1; }
            [[nodiscard]] std::uint32_t GetChannels() const override { return 4; }
        };

        std::set<std::string> names;
        FakeTexture texture;

        void SetInt(const std::string &name, int) override { names.insert(name); }
        void SetFloat(const std::string &name, float) override { names.insert(name); }
        void SetVec2(const std::string &name, float, float) override { names.insert(name); }
        void SetVec2(const std::string &name, N2Engine::Math::Vector2 &) override { names.insert(name); }
        void SetVec3(const std::string &name, float, float, float) override { names.insert(name); }
        void SetVec3(const std::string &name, N2Engine::Math::Vector3 &) override { names.insert(name); }
        void SetVec4(const std::string &name, float, float, float, float) override { names.insert(name); }
        void SetVec4(const std::string &name, N2Engine::Math::Vector4 &) override { names.insert(name); }
        void SetColor(const std::string &name, float, float, float, float) override { names.insert(name); }
        void SetTexture(Renderer::Common::ITexture *) override {}

        [[nodiscard]] Renderer::Common::IShader *GetShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::ITexture *GetTexture() const override
        {
            return const_cast<FakeTexture *>(&texture);
        }
        [[nodiscard]] Renderer::Common::ITexture *GetAuxTexture(Renderer::Common::AuxTexture) const override
        {
            return const_cast<FakeTexture *>(&texture);
        }
        [[nodiscard]] bool IsValid() const override { return true; }
    };

    /// Runs of whitespace (newlines, indentation) as one space, so a reformatted shader still matches the needles
    std::string Normalised(const std::string &text)
    {
        std::string out;
        bool space = false;
        for (const char c : text)
        {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                space = !out.empty();
                continue;
            }
            if (space)
            {
                out.push_back(' ');
                space = false;
            }
            out.push_back(c);
        }
        return out;
    }

    bool Contains(const std::string &source, const std::string &needle)
    {
        return Normalised(source).find(Normalised(needle)) != std::string::npos;
    }

    /// The number after `name = ` in GLSL source, or -1
    double ConstantIn(const std::string &source, const std::string &name)
    {
        const std::size_t at = source.find(name + " = ");
        if (at == std::string::npos)
        {
            return -1.0;
        }
        return std::stod(source.substr(at + name.size() + 3));
    }
}

TEST(OpenGlShaderSourceTest, TheSourcesAreFound)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    ASSERT_FALSE(renderer.empty()) << "N2_RENDERER_SOURCE_DIR is " << N2_RENDERER_SOURCE_DIR;
    EXPECT_FALSE(ShaderSource(renderer, "litFrag").empty());
    EXPECT_FALSE(ShaderSource(renderer, "litVert").empty());
    EXPECT_FALSE(ShaderSource(renderer, "unlitFrag").empty());
}

TEST(OpenGlShaderSourceTest, EveryUniformAMaterialSetsIsDeclaredByTheShaderItDrawsWith)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string litFrag = ShaderSource(renderer, "litFrag");
    const std::string unlitFrag = ShaderSource(renderer, "unlitFrag");
    ASSERT_FALSE(litFrag.empty());
    ASSERT_FALSE(unlitFrag.empty());

    const auto texture = Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4});
    for (const ShadingModel shading : {ShadingModel::Lit, ShadingModel::Pbr})
    {
        const auto material = Material::Create(shading);
        material->SetBaseColorTexture(texture);
        RecordingMaterial recorded;
        material->ApplyUniforms(recorded);
        ASSERT_GT(recorded.names.size(), 10u);
        for (const std::string &name : recorded.names)
        {
            EXPECT_TRUE(DeclaresUniform(litFrag, name)) << "the lit fragment shader doesn't declare " << name;
        }
        // The extras this PR adds are among them
        for (const char *name : {"uPbr", "uHasNormalTexture", "uNormalScale", "uHasMetallicRoughnessTexture",
                                 "uMetallic", "uSmoothness"})
        {
            EXPECT_TRUE(recorded.names.contains(name)) << name;
        }
    }

    const auto unlit = Material::Create(ShadingModel::Unlit);
    RecordingMaterial recorded;
    unlit->ApplyUniforms(recorded);
    ASSERT_FALSE(recorded.names.empty());
    for (const std::string &name : recorded.names)
    {
        EXPECT_TRUE(DeclaresUniform(unlitFrag, name)) << "the unlit fragment shader doesn't declare " << name;
    }
    EXPECT_FALSE(recorded.names.contains("uPbr")) << "an unlit material sets no lit-shader uniform";
}

TEST(OpenGlShaderSourceTest, EveryUniformTheLitShaderTakesFromAMaterialHasADefaultWhenAMaterialIsMadeWithoutOne)
{
    // The shared program keeps the last value any material gave a uniform, so a material nobody called ApplyUniforms on
    // (CreateMaterial alone) must set every one of them itself: a declared uniform without a default is a leak
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string material = ReadFile("src/opengl/OpenGLMaterial.cpp");
    const std::size_t from = renderer.find("OpenGLRenderer::CreateMaterial(");
    const std::size_t to = renderer.find("OpenGLRenderer::DestroyMaterial(");
    ASSERT_NE(from, std::string::npos);
    ASSERT_NE(to, std::string::npos);
    const std::string created = renderer.substr(from, to - from) + material; // the constructor sets uAlbedo

    RecordingMaterial recorded;
    Material::Create(ShadingModel::Pbr)->ApplyUniforms(recorded);
    for (const std::string &name : recorded.names)
    {
        EXPECT_NE(created.find("\"" + name + "\""), std::string::npos) << name << " has no default in CreateMaterial";
    }
}

TEST(OpenGlShaderSourceTest, TheSamplersAndTheirTextureUnitsLineUpWithTheMaterialsBinding)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string material = ReadFile("src/opengl/OpenGLMaterial.cpp");
    const std::string litFrag = ShaderSource(renderer, "litFrag");

    // The enum order the unit formula (1 + value) depends on
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Emissive) == 0);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Occlusion) == 1);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Normal) == 2);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::MetallicRoughness) == 3);
    EXPECT_TRUE(Contains(material, "return 1 + static_cast<int>(which);"));

    for (const char *sampler : {"uEmissiveTexture", "uOcclusionTexture", "uNormalTexture", "uMetallicRoughnessTexture"})
    {
        EXPECT_TRUE(DeclaresUniform(litFrag, sampler)) << sampler << " in the shader";
        EXPECT_TRUE(Contains(material, std::string("return \"") + sampler + "\";"))
            << sampler << " in OpenGLMaterial::AuxSamplerName";
        EXPECT_TRUE(Contains(litFrag, std::string("uniform sampler2D ") + sampler + ";")) << sampler << " is a sampler2D";
    }
    // The renderer binds every extra texture to its unit
    EXPECT_TRUE(Contains(renderer, "GL_TEXTURE0 + static_cast<GLenum>(OpenGLMaterial::AuxTextureUnit(which))"));
    EXPECT_TRUE(Contains(renderer, "Common::AuxTexture::MetallicRoughness})"));
}

TEST(OpenGlShaderSourceTest, TheTangentIsAttributeFourOfBothTheShaderAndTheMeshAndPassedBetweenTheStages)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string mesh = ReadFile("src/opengl/OpenGLMesh.cpp");
    const std::string litVert = ShaderSource(renderer, "litVert");
    const std::string litFrag = ShaderSource(renderer, "litFrag");
    EXPECT_TRUE(Contains(litVert, "layout (location = 4) in vec4 aTangent;"));
    EXPECT_TRUE(Contains(litVert, "out vec4 fragTangent;"));
    EXPECT_TRUE(Contains(litFrag, "in vec4 fragTangent;"));

    EXPECT_TRUE(Contains(mesh, "glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Common::Vertex),"));
    EXPECT_TRUE(Contains(mesh, "offsetof(Common::Vertex, tangent)"));
    EXPECT_TRUE(Contains(mesh, "glEnableVertexAttribArray(4);"));
    // The other attributes are where the shaders put them
    EXPECT_TRUE(Contains(litVert, "layout (location = 0) in vec3 aPos;"));
    EXPECT_TRUE(Contains(litVert, "layout (location = 3) in vec4 aColor;"));
}

TEST(OpenGlShaderSourceTest, ThePbrConstantsAreTheSoftwareRenderersAndPbrOnlyInputsAreGatedOnUPbr)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string litFrag = ShaderSource(renderer, "litFrag");
    EXPECT_NEAR(ConstantIn(litFrag, "MIN_PERCEPTUAL_ROUGHNESS"), Renderer::Common::kMinPerceptualRoughness, 1e-6);
    EXPECT_NEAR(ConstantIn(litFrag, "DIELECTRIC_F0"), Renderer::Common::kDielectricF0, 1e-6);
    EXPECT_NEAR(ConstantIn(litFrag, "PI"), Renderer::Common::kPi, 1e-6);
    // The environment BRDF fit's coefficients
    for (const char *coefficient : {"vec4(-1.0, -0.0275, -0.572, 0.022)", "vec4(1.0, 0.0425, 1.04, -0.04)",
                                    "exp2(-9.28 * nDotV)", "vec2(-1.04, 1.04)"})
    {
        EXPECT_TRUE(Contains(litFrag, coefficient)) << coefficient;
    }
    // A Lit material never samples the metallic-roughness texture
    EXPECT_TRUE(Contains(litFrag, "if (uPbr != 0 && uHasMetallicRoughnessTexture)"));
    // A material made without ApplyUniforms is plain Blinn-Phong with nothing extra
    for (const char *reset : {"material->SetInt(\"uPbr\", 0);", "material->SetInt(\"uHasNormalTexture\", 0);",
                              "material->SetInt(\"uHasMetallicRoughnessTexture\", 0);",
                              "material->SetFloat(\"uNormalScale\", 1.0f);"})
    {
        EXPECT_TRUE(Contains(renderer, reset)) << reset;
    }
}
