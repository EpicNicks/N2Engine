#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <renderer/common/GpuShaderInterface.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/UserShader.hpp>
#include <renderer/common/UserShaderGl.hpp>

#include "common/UserShaderFixture.hpp"

// The OpenGL half of the user shaders (orchestrator/proposals/user-shaders.md, stage 4), as TEXT: OpenGL cannot run in CI, so what
// is pinned here is everything that can be checked without a context. The cooked fragment text of every fixture (the OpenGL
// prelude, the manifest's parameters as plain uniforms, the author's stage) against the vertex stage and the uniform names the
// renderer sets by name (renderer/common/UserShaderGl.hpp, which OpenGLRenderer takes them from), the manifest, the limits on the
// text before a driver sees it, and the renderer's own source for the by-name per-draw and lighting uniforms. That the driver
// compiles the text and the pixels are right is for a GPU: see the PR's checklist.

using namespace UserShaderTesting;
using Renderer::Common::UserParamType;
using Renderer::Common::UserShaderBlob;
using Renderer::Common::UserShaderTarget;
using Renderer::Common::ValidateUserShaderBlob;
namespace UserGl = Renderer::Common::UserGl;

namespace
{
    const char* const kFixtures[] = {"solid", "textured", "ambient", "position", "varyings"};

    std::string ReadText(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        EXPECT_TRUE(static_cast<bool>(file)) << path;
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    }

    std::string RendererSource(const char* relative)
    {
        return ReadText(std::string(N2_RENDERER_SOURCE_DIR) + "/" + relative);
    }

    /// The type of the plain uniform `name` declared in `glsl` (`uniform <type> name;` or `name[N];`), or an empty string
    std::string UniformType(const std::string& glsl, const std::string& name)
    {
        const std::regex pattern("(^|\\n)\\s*uniform\\s+(\\w+)\\s+" + name + "\\s*(\\[\\s*\\d+\\s*\\])?\\s*;");
        std::smatch match;
        return std::regex_search(glsl, match, pattern) ? match[2].str() : std::string();
    }

    const char* GlslTypeOf(const UserParamType type)
    {
        switch (type)
        {
        case UserParamType::Float: return "float";
        case UserParamType::Int:
        case UserParamType::Bool: return "int"; // a bool is declared as an int
        case UserParamType::Vec2: return "vec2";
        case UserParamType::Vec3: return "vec3";
        case UserParamType::Vec4:
        case UserParamType::Color: return "vec4";
        case UserParamType::Texture: return "";
        }
        return "";
    }

    /// Every `in|out <type> <name>;` of `text` as "<type> <name>"
    std::set<std::string> Interface(const std::string& text, const char* direction)
    {
        std::set<std::string> out;
        const std::regex pattern(std::string("(^|\\n)\\s*") + direction + "\\s+(\\w+)\\s+(\\w+)\\s*;");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it)
        {
            out.insert((*it)[2].str() + " " + (*it)[3].str());
        }
        return out;
    }

    bool Contains(const std::string& text, const std::string& what)
    {
        return text.find(what) != std::string::npos;
    }
}

TEST(UserShaderGlTest, EveryCookedFixtureCarriesAGlTextThatTheOpenGlTargetAccepts)
{
    for (const char* name : kFixtures)
    {
        const UserShaderBlob blob = LoadFixture(name);
        EXPECT_EQ(ValidateUserShaderBlob(blob, UserShaderTarget::OpenGl), "") << name;
        EXPECT_EQ(blob.fragmentGlsl.rfind("#version 330 core", 0), 0u) << name;
        EXPECT_FALSE(Contains(blob.fragmentGlsl, "#extension")) << name << ": the GL text needs no extension";
    }
}

TEST(UserShaderGlTest, ATextTheDriverShouldNeverSeeIsRefused)
{
    const auto check = [](const std::string& text)
    {
        UserShaderBlob blob = LoadFixture("solid");
        blob.fragmentGlsl = text;
        return ValidateUserShaderBlob(blob, UserShaderTarget::OpenGl);
    };
    const std::string good = LoadFixture("solid").fragmentGlsl;
    EXPECT_EQ(check(good), "");
    EXPECT_NE(check("").find("empty"), std::string::npos);
    EXPECT_NE(check("void main() {}").find("#version 330 core"), std::string::npos);
    EXPECT_NE(check("#version 450\nvoid main() {}").find("#version 330 core"), std::string::npos) << "another version";
    EXPECT_NE(check(good + "\n#version 450\n").find("second #version"), std::string::npos);
    EXPECT_NE(check(good + "\n  #extension GL_ARB_gpu_shader5 : enable\n").find("#extension"), std::string::npos);
    EXPECT_NE(check(good + std::string("\0x", 2)).find("NUL"), std::string::npos);
    EXPECT_NE(check("#version 330 core\nuniform float a;\n").find("no main"), std::string::npos);
    EXPECT_NE(check(good + std::string(Renderer::Common::kUserShaderMaxGlslBytes, ' ')).find("limit"), std::string::npos);
    // The parameters are checked whatever the target
    UserShaderBlob blob = LoadFixture("solid");
    blob.blockSize = 4096;
    EXPECT_NE(ValidateUserShaderBlob(blob, UserShaderTarget::OpenGl), "");
    // Another target's payload is not this one's
    blob = LoadFixture("solid");
    blob.fragmentGlsl.clear();
    EXPECT_NE(ValidateUserShaderBlob(blob, UserShaderTarget::OpenGl), "");
    EXPECT_EQ(ValidateUserShaderBlob(blob), "") << "the Vulkan payload is untouched";
}

TEST(UserShaderGlTest, EveryPerDrawUniformTheRendererSetsIsDeclaredByThePrelude)
{
    for (const char* name : kFixtures)
    {
        const std::string glsl = LoadFixture(name).fragmentGlsl;
        for (const std::string& uniform : UserGl::PerDrawUniformNames())
        {
            EXPECT_FALSE(UniformType(glsl, uniform).empty()) << name << ": " << uniform << " is not declared";
        }
        EXPECT_EQ(UniformType(glsl, UserGl::kModel), "mat4");
        EXPECT_EQ(UniformType(glsl, UserGl::kStageDim), "float");
        EXPECT_EQ(UniformType(glsl, UserGl::kTintInside), "int");
        EXPECT_EQ(UniformType(glsl, UserGl::kTintCircle), "vec3");
        EXPECT_EQ(UniformType(glsl, UserGl::kTargetHeight), "float") << "n2_FragCoord() flips gl_FragCoord with it";
    }
}

TEST(UserShaderGlTest, EachDeclaredParameterIsAPlainUniformOfItsGlslTypeAndEachTextureASampler)
{
    for (const char* name : kFixtures)
    {
        const UserShaderBlob blob = LoadFixture(name);
        std::set<unsigned> slots;
        for (const Renderer::Common::UserParam& param : blob.params)
        {
            if (param.type == UserParamType::Texture)
            {
                slots.insert(param.slot);
                // a texture parameter is the slot number (what n2_texture takes), and its sampler is n2_tex<slot>
                EXPECT_TRUE(Contains(blob.fragmentGlsl, "const int " + param.name + " = " + std::to_string(param.slot) + ";"))
                    << name << ": " << param.name;
                EXPECT_EQ(UniformType(blob.fragmentGlsl, UserGl::TextureSamplerName(param.slot)), "sampler2D") << name;
            }
            else
            {
                EXPECT_EQ(UniformType(blob.fragmentGlsl, param.name), GlslTypeOf(param.type)) << name << ": " << param.name;
            }
        }
        for (unsigned slot = 0; slot < Renderer::Common::kUserShaderTextureSlots; ++slot)
        {
            if (slots.count(slot) == 0)
            {
                EXPECT_TRUE(UniformType(blob.fragmentGlsl, UserGl::TextureSamplerName(slot)).empty())
                    << name << ": slot " << slot << " has a sampler but no texture parameter";
            }
        }
    }
}

TEST(UserShaderGlTest, TheLightingUniformsAreDeclaredOnlyForAShaderThatAsksForThem)
{
    const std::string lit = LoadFixture("ambient").fragmentGlsl;
    EXPECT_TRUE(LoadFixture("ambient").usesLighting);
    const UserGl::LightingNames& names = UserGl::kUserLightingNames;
    EXPECT_EQ(UniformType(lit, names.encodeOutput), "int");
    EXPECT_EQ(UniformType(lit, names.ambient), "vec3");
    EXPECT_EQ(UniformType(lit, names.cameraPos), "vec3");
    EXPECT_EQ(UniformType(lit, names.numDirectional), "int");
    EXPECT_EQ(UniformType(lit, names.numPoint), "int");
    EXPECT_EQ(UniformType(lit, names.numSpot), "int");
    EXPECT_EQ(UniformType(lit, names.directionalArray), "N2DirectionalLight");
    EXPECT_EQ(UniformType(lit, names.pointArray), "N2PointLight");
    EXPECT_EQ(UniformType(lit, names.spotArray), "N2SpotLight");

    // The arrays' sizes are the scene lighting's maxima, and each member the renderer sets by "array[i].member" is a member of the
    // struct (the names the renderer builds are the ones of UserGl::LightMember)
    EXPECT_TRUE(Contains(lit, std::string(names.directionalArray) + "[" + std::to_string(Renderer::Common::SceneLightingData::MAX_DIRECTIONAL_LIGHTS) + "]"));
    EXPECT_TRUE(Contains(lit, std::string(names.pointArray) + "[" + std::to_string(Renderer::Common::SceneLightingData::MAX_POINT_LIGHTS) + "]"));
    EXPECT_TRUE(Contains(lit, std::string(names.spotArray) + "[" + std::to_string(Renderer::Common::SceneLightingData::MAX_SPOT_LIGHTS) + "]"));
    const std::regex member("^(\\w+)\\[\\d+\\]\\.(\\w+)$");
    std::set<std::string> structs;
    for (const std::string& uniform : UserGl::LightingUniformNames())
    {
        std::smatch match;
        if (!std::regex_match(uniform, match, member))
        {
            EXPECT_FALSE(UniformType(lit, uniform).empty()) << uniform;
            continue;
        }
        const std::string structName = match[1] == names.directionalArray ? "N2DirectionalLight"
            : match[1] == names.pointArray                                ? "N2PointLight"
                                                                          : "N2SpotLight";
        const std::regex field("struct\\s+" + structName + "\\s*\\{[^}]*\\b" + match[2].str() + "\\s*;");
        EXPECT_TRUE(std::regex_search(lit, field)) << uniform << ": " << structName << " has no member " << match[2].str();
    }

    for (const char* name : {"solid", "textured", "position", "varyings"})
    {
        const std::string glsl = LoadFixture(name).fragmentGlsl;
        EXPECT_FALSE(LoadFixture(name).usesLighting);
        for (const std::string& uniform : {std::string(names.ambient), std::string(names.cameraPos), std::string(names.directionalArray)})
        {
            EXPECT_TRUE(UniformType(glsl, uniform).empty()) << name << " declares the lighting uniform " << uniform << " without asking";
        }
    }
}

TEST(UserShaderGlTest, TheEngineVertexStageWritesTheVaryingsThePreludeReadsAndNothingElse)
{
    const std::string vertex = UserGl::VertexSource();
    EXPECT_EQ(vertex.rfind("#version 330 core", 0), 0u);
    const std::set<std::string> outs = Interface(vertex, "out");
    for (const char* name : kFixtures)
    {
        const std::set<std::string> ins = Interface(LoadFixture(name).fragmentGlsl, "in");
        EXPECT_EQ(ins, outs) << name << ": the fragment inputs are matched by name and type with the vertex outputs";
    }
    EXPECT_EQ(outs.size(), 5u);
    for (const char* varying : {"vec2 n2_UV", "vec4 n2_Color", "vec3 n2_WorldPos", "vec3 n2_Normal", "vec4 n2_Tangent"})
    {
        EXPECT_EQ(outs.count(varying), 1u) << varying;
    }

    // The attributes are the Common::Vertex ones at the locations of the interface table (position 0 ... tangent 4)
    for (const Renderer::Gpu::ShaderInterface::Attribute& attribute : Renderer::Gpu::ShaderInterface::Attributes(Renderer::Gpu::ShaderInterface::BuiltInShader::Lit))
    {
        const std::string declaration = "layout (location = " + std::to_string(attribute.location) + ") in ";
        EXPECT_TRUE(Contains(vertex, declaration)) << "location " << attribute.location;
    }
    // The vertex stage declares the three matrices under the contract's names, and OpenGL's clip space needs no z remap
    EXPECT_EQ(UniformType(vertex, UserGl::kModel), "mat4");
    EXPECT_EQ(UniformType(vertex, UserGl::kView), "mat4");
    EXPECT_EQ(UniformType(vertex, UserGl::kProjection), "mat4");
    EXPECT_FALSE(Contains(vertex, "gl_Position.z")) << "the z remap is Vulkan's and WebGPU's";
    EXPECT_TRUE(Contains(vertex, "n2_Projection * n2_View * worldPos"));
}

TEST(UserShaderGlTest, TheRenderersOwnSourceSetsTheUserUniformsByTheNamesOfTheSharedHeader)
{
    const std::string shader = RendererSource("src/opengl/OpenGLShader.cpp");
    const std::string renderer = RendererSource("src/opengl/OpenGLRenderer.cpp");
    const std::string material = RendererSource("src/opengl/OpenGLMaterial.cpp");

    // The per-draw uniforms: the program looks up the contract's names next to the built-ins' (so the draw path, which sets them
    // by location, serves both), and the draw sets the target's height
    for (const char* name : {"n2_Model", "n2_View", "n2_Projection", "n2_StageDim", "n2_TintSaturation", "n2_TintInside", "n2_TintCircle", "n2_TargetHeight"})
    {
        EXPECT_TRUE(Contains(shader, std::string("\"") + name + "\"")) << name << " is not looked up by OpenGLShader";
    }
    EXPECT_TRUE(Contains(renderer, "targetHeightLoc"));
    EXPECT_TRUE(Contains(renderer, "glUniform1f(uniforms.targetHeightLoc"));

    // The lighting: the user shaders' uniforms are set from the names of the shared header; the built-in lit shader's code is the one
    // VulkanShaderParityTests reads
    EXPECT_TRUE(Contains(renderer, "UserGl::kUserLightingNames"));
    EXPECT_TRUE(Contains(renderer, "UserGl::LightMember"));
    EXPECT_TRUE(Contains(renderer, "\"uDirectionalLights[\"")) << "the built-in lit shader keeps its own code and names";
    for (const char* member : {"direction", "color", "intensity", "position", "range", "attenuation", "cosInner", "cosOuter"})
    {
        EXPECT_TRUE(Contains(renderer, std::string("\"") + member + "\"")) << member;
    }

    // The vertex stage and the samplers come from the shared header too
    EXPECT_TRUE(Contains(renderer, "UserGl::VertexSource()"));
    EXPECT_TRUE(Contains(material, "UserGl::TextureSamplerName"));
}
