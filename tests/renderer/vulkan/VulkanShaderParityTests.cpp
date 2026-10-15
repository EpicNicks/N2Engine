#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/vulkan/VulkanShaderInterface.hpp>

#include "engine/rendering/Material.hpp"
#include "engine/rendering/Texture.hpp"

// The parity test of the Vulkan backend's shaders (#43, stage B), modelled on OpenGlShaderSourceTests: it needs NO Vulkan
// device (so it runs on every leg that builds the Vulkan backend, the Windows ones too) because it reads the GLSL under
// renderer/shaders/vulkan/ as TEXT. It ties together, for the three built-in shaders:
//   - every uniform Material::ApplyUniforms sets for the shader GpuCache would pick (and every default
//     OpenGLRenderer::CreateMaterial sets, and every uniform the GL shaders declare) has an entry in the interface table
//     (VulkanShaderInterface.hpp) AND is declared as a member of the right block in the GLSL;
//   - the table's std140 offsets, sizes and member order are the ones computed from the GLSL block declarations (a small
//     std140 calculator: scalars, vectors, mat4, and the light structs and their arrays);
//   - the vertex attribute locations of the GLSL are the table's and Common::Vertex's offsets, and what the vertex stage
//     outputs is what the fragment stage takes;
//   - the sampler bindings are 1-5 in AuxTexture order after the base texture;
//   - the lit port keeps the PBR constants and the environment-BRDF coefficients, the text port the antialiased SDF maths,
//     and every vertex shader does the clip-space remap.
// A renamed or forgotten uniform, a reordered block member or a moved binding fails here instead of drawing wrong colours
// (an unknown uniform name is otherwise silently ignored).

using namespace N2Engine;
using Rendering::Material;
using Rendering::ShadingModel;
using Rendering::Texture;
namespace Vk = Renderer::Vulkan::ShaderInterface;

namespace
{
    std::string ReadFile(const std::string& relative)
    {
        std::ifstream file(std::string(N2_RENDERER_SOURCE_DIR) + "/" + relative, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    /// Runs of whitespace (newlines, indentation) as one space, so a reformatted shader still matches the needles
    std::string Normalised(const std::string& text)
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

    bool Contains(const std::string& source, const std::string& needle)
    {
        return Normalised(source).find(Normalised(needle)) != std::string::npos;
    }

    /// The number after `name = ` in GLSL source, or -1
    double ConstantIn(const std::string& source, const std::string& name)
    {
        const std::size_t at = source.find(name + " = ");
        if (at == std::string::npos)
        {
            return -1.0;
        }
        return std::stod(source.substr(at + name.size() + 3));
    }

    std::string StripComments(const std::string& source)
    {
        std::string out;
        std::size_t i = 0;
        while (i < source.size())
        {
            if (source.compare(i, 2, "//") == 0)
            {
                while (i < source.size() && source[i] != '\n')
                {
                    ++i;
                }
            }
            else if (source.compare(i, 2, "/*") == 0)
            {
                const std::size_t end = source.find("*/", i + 2);
                i = end == std::string::npos ? source.size() : end + 2;
            }
            else
            {
                out.push_back(source[i++]);
            }
        }
        return out;
    }

    // ---- A very small GLSL reader: uniform blocks, structs, samplers and stage inputs/outputs ----------------------
    struct Member
    {
        std::string type;
        std::string name;
        std::uint32_t arrayCount = 1;
    };

    struct UniformBlock
    {
        std::string name;
        std::string layout; // the text inside layout( ... )
        std::uint32_t set = 0;
        std::uint32_t binding = 0;
        std::vector<Member> members;
    };

    struct SamplerDeclaration
    {
        std::uint32_t set = 0;
        std::uint32_t binding = 0;
    };

    struct StageVariable
    {
        std::string direction; // "in" or "out"
        std::uint32_t location = 0;
        std::string type;
        std::string name;
    };

    struct Glsl
    {
        std::string text; // comments removed
        std::vector<UniformBlock> blocks;
        std::map<std::string, std::vector<Member>> structs;
        std::map<std::string, SamplerDeclaration> samplers;
        std::vector<StageVariable> variables;

        [[nodiscard]] const UniformBlock* Block(const std::string& name) const
        {
            for (const UniformBlock& block : blocks)
            {
                if (block.name == name)
                {
                    return &block;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const StageVariable* Variable(const std::string& direction, const std::uint32_t location) const
        {
            for (const StageVariable& variable : variables)
            {
                if (variable.direction == direction && variable.location == location)
                {
                    return &variable;
                }
            }
            return nullptr;
        }
    };

    std::uint32_t NumberAfter(const std::string& text, const std::string& key)
    {
        const std::regex pattern(key + "\\s*=\\s*(\\d+)");
        std::smatch match;
        if (std::regex_search(text, match, pattern))
        {
            return static_cast<std::uint32_t>(std::stoul(match[1].str()));
        }
        return 0xFFFFFFFFu;
    }

    std::vector<Member> ParseMembers(const std::string& body)
    {
        std::vector<Member> members;
        const std::regex declaration(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*(\d+)\s*\])?\s*$)");
        std::size_t start = 0;
        while (start < body.size())
        {
            std::size_t end = body.find(';', start);
            if (end == std::string::npos)
            {
                end = body.size();
            }
            const std::string piece = body.substr(start, end - start);
            start = end + 1;
            std::smatch match;
            if (std::regex_match(piece, match, declaration))
            {
                Member member;
                member.type = match[1].str();
                member.name = match[2].str();
                member.arrayCount = match[3].matched ? static_cast<std::uint32_t>(std::stoul(match[3].str())) : 1u;
                members.push_back(member);
            }
        }
        return members;
    }

    Glsl ParseGlsl(const std::string& relative)
    {
        Glsl glsl;
        glsl.text = StripComments(ReadFile(relative));
        const std::string& text = glsl.text;

        const std::regex block(R"(layout\s*\(([^)]*)\)\s*uniform\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\}\s*;)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), block); it != std::sregex_iterator(); ++it)
        {
            UniformBlock parsed;
            parsed.layout = (*it)[1].str();
            parsed.name = (*it)[2].str();
            parsed.set = NumberAfter(parsed.layout, "set");
            parsed.binding = NumberAfter(parsed.layout, "binding");
            parsed.members = ParseMembers((*it)[3].str());
            glsl.blocks.push_back(parsed);
        }

        const std::regex structure(R"(struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\}\s*;)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), structure); it != std::sregex_iterator(); ++it)
        {
            glsl.structs[(*it)[1].str()] = ParseMembers((*it)[2].str());
        }

        const std::regex sampler(R"(layout\s*\(([^)]*)\)\s*uniform\s+sampler2D\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), sampler); it != std::sregex_iterator(); ++it)
        {
            SamplerDeclaration declaration;
            declaration.set = NumberAfter((*it)[1].str(), "set");
            declaration.binding = NumberAfter((*it)[1].str(), "binding");
            glsl.samplers[(*it)[2].str()] = declaration;
        }

        const std::regex variable(
            R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*(in|out)\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), variable); it != std::sregex_iterator(); ++it)
        {
            StageVariable parsed;
            parsed.location = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
            parsed.direction = (*it)[2].str();
            parsed.type = (*it)[3].str();
            parsed.name = (*it)[4].str();
            glsl.variables.push_back(parsed);
        }
        return glsl;
    }

    // ---- std140 -----------------------------------------------------------------------------------------------------
    struct Layout
    {
        std::uint32_t alignment = 0;
        std::uint32_t size = 0;
        bool known = true;
    };

    std::uint32_t AlignUp(const std::uint32_t value, const std::uint32_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    Layout Std140Layout(const std::string& type, const std::uint32_t arrayCount, const Glsl& glsl);

    Layout StructLayout(const std::vector<Member>& members, const Glsl& glsl)
    {
        Layout result;
        std::uint32_t cursor = 0;
        std::uint32_t maxAlignment = 16; // a std140 struct aligns like a vec4 at least
        for (const Member& member : members)
        {
            const Layout layout = Std140Layout(member.type, member.arrayCount, glsl);
            if (!layout.known)
            {
                result.known = false;
                return result;
            }
            cursor = AlignUp(cursor, layout.alignment) + layout.size;
            maxAlignment = std::max(maxAlignment, layout.alignment);
        }
        result.alignment = maxAlignment;
        result.size = AlignUp(cursor, maxAlignment);
        return result;
    }

    Layout Std140Layout(const std::string& type, const std::uint32_t arrayCount, const Glsl& glsl)
    {
        Layout element;
        if (type == "int" || type == "uint" || type == "float" || type == "bool")
        {
            element = {4, 4, true};
        }
        else if (type == "vec2" || type == "ivec2")
        {
            element = {8, 8, true};
        }
        else if (type == "vec3" || type == "ivec3")
        {
            element = {16, 12, true};
        }
        else if (type == "vec4" || type == "ivec4")
        {
            element = {16, 16, true};
        }
        else if (type == "mat4")
        {
            element = {16, 64, true}; // four vec4 columns (or rows, with row_major)
        }
        else if (const auto it = glsl.structs.find(type); it != glsl.structs.end())
        {
            element = StructLayout(it->second, glsl);
        }
        else
        {
            return Layout{0, 0, false};
        }
        if (!element.known)
        {
            return element;
        }
        if (arrayCount == 1)
        {
            return element;
        }
        // An array's element stride is the element size rounded up to a vec4, and the array aligns like a vec4
        const std::uint32_t stride = AlignUp(element.size, 16);
        return Layout{AlignUp(element.alignment, 16), stride * arrayCount, true};
    }

    struct PlacedMember
    {
        std::string name;
        std::string type;
        std::uint32_t arrayCount = 1;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    /// The members of a block at the std140 offsets the GLSL declaration gives them; `end` is where the last one ends
    std::vector<PlacedMember> PlaceBlock(const UniformBlock& block, const Glsl& glsl, std::uint32_t& end, bool& known)
    {
        std::vector<PlacedMember> placed;
        std::uint32_t cursor = 0;
        known = true;
        for (const Member& member : block.members)
        {
            const Layout layout = Std140Layout(member.type, member.arrayCount, glsl);
            if (!layout.known)
            {
                known = false;
                break;
            }
            PlacedMember entry;
            entry.name = member.name;
            entry.type = member.type;
            entry.arrayCount = member.arrayCount;
            entry.offset = AlignUp(cursor, layout.alignment);
            entry.size = layout.size;
            cursor = entry.offset + entry.size;
            placed.push_back(entry);
        }
        end = cursor;
        return placed;
    }

    const char* GlslTypeOf(const Vk::UniformType type)
    {
        switch (type)
        {
        case Vk::UniformType::Int: return "int";
        case Vk::UniformType::Float: return "float";
        case Vk::UniformType::Vec2: return "vec2";
        case Vk::UniformType::Vec3: return "vec3";
        case Vk::UniformType::Vec4: return "vec4";
        case Vk::UniformType::Mat4: return "mat4";
        case Vk::UniformType::Struct: return "";
        }
        return "";
    }

    std::uint32_t ComponentsOf(const std::string& type)
    {
        if (type == "vec2") return 2;
        if (type == "vec3") return 3;
        if (type == "vec4") return 4;
        if (type == "float") return 1;
        return 0;
    }

    /// Checks one GLSL block against a table: same members in the same order, same types, std140 offsets and sizes
    void ExpectBlockMatchesTable(const Glsl& glsl, const std::string& blockName, const std::uint32_t set,
                                 const std::uint32_t binding, const std::span<const Vk::Uniform> table,
                                 const std::string& context)
    {
        const UniformBlock* block = glsl.Block(blockName);
        ASSERT_NE(block, nullptr) << context << ": no uniform block " << blockName;
        EXPECT_EQ(block->set, set) << context << ": " << blockName << "'s set";
        EXPECT_EQ(block->binding, binding) << context << ": " << blockName << "'s binding";
        EXPECT_NE(block->layout.find("std140"), std::string::npos) << context << ": " << blockName << " is std140";

        std::uint32_t end = 0;
        bool known = false;
        const std::vector<PlacedMember> placed = PlaceBlock(*block, glsl, end, known);
        ASSERT_TRUE(known) << context << ": the std140 calculator does not know a type in " << blockName;
        ASSERT_EQ(placed.size(), table.size()) << context << ": " << blockName << " has another number of members";
        for (std::size_t i = 0; i < table.size(); ++i)
        {
            const Vk::Uniform& expected = table[i];
            const PlacedMember& actual = placed[i];
            EXPECT_EQ(actual.name, std::string(expected.name)) << context << ": member " << i << " of " << blockName;
            EXPECT_EQ(actual.offset, expected.offset) << context << ": offset of " << expected.name;
            EXPECT_EQ(actual.size, expected.size) << context << ": size of " << expected.name;
            EXPECT_EQ(actual.arrayCount, expected.arrayCount) << context << ": array count of " << expected.name;
            if (expected.type != Vk::UniformType::Struct)
            {
                EXPECT_EQ(actual.type, std::string(GlslTypeOf(expected.type))) << context << ": type of " << expected.name;
            }
            else
            {
                EXPECT_TRUE(glsl.structs.contains(actual.type)) << context << ": " << expected.name << " is a struct";
            }
        }
    }

    bool BlockDeclares(const Glsl& glsl, const std::string& blockName, const std::string& member)
    {
        const UniformBlock* block = glsl.Block(blockName);
        if (block == nullptr)
        {
            return false;
        }
        for (const Member& candidate : block->members)
        {
            if (candidate.name == member)
            {
                return true;
            }
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

        void SetInt(const std::string& name, int) override { names.insert(name); }
        void SetFloat(const std::string& name, float) override { names.insert(name); }
        void SetVec2(const std::string& name, float, float) override { names.insert(name); }
        void SetVec2(const std::string& name, N2Engine::Math::Vector2&) override { names.insert(name); }
        void SetVec3(const std::string& name, float, float, float) override { names.insert(name); }
        void SetVec3(const std::string& name, N2Engine::Math::Vector3&) override { names.insert(name); }
        void SetVec4(const std::string& name, float, float, float, float) override { names.insert(name); }
        void SetVec4(const std::string& name, N2Engine::Math::Vector4&) override { names.insert(name); }
        void SetColor(const std::string& name, float, float, float, float) override { names.insert(name); }
        void SetTexture(Renderer::Common::ITexture*) override {}

        [[nodiscard]] Renderer::Common::IShader* GetShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::ITexture* GetTexture() const override
        {
            return const_cast<FakeTexture*>(&texture);
        }
        [[nodiscard]] Renderer::Common::ITexture* GetAuxTexture(Renderer::Common::AuxTexture) const override
        {
            return const_cast<FakeTexture*>(&texture);
        }
        [[nodiscard]] bool IsValid() const override { return true; }
    };

    /// The text of a raw-string shader in OpenGLRenderer.cpp: `const char *<name> = R"(` ... `)";`
    std::string GlShaderSource(const std::string& file, const std::string& name)
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

    /// The names of the non-sampler uniforms a GL shader declares (a struct array counts by its array's name)
    std::set<std::string> GlUniformNames(const std::string& rawSource)
    {
        // Comments away first: "non-uniform control flow" is not a declaration
        const std::string source = StripComments(rawSource);
        std::set<std::string> names;
        const std::regex declaration(R"(uniform\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*))");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration); it != std::sregex_iterator(); ++it)
        {
            if ((*it)[1].str() != "sampler2D")
            {
                names.insert((*it)[2].str());
            }
        }
        return names;
    }

    /// The quoted uniform names ("uXxx") in a piece of C++ source
    std::set<std::string> QuotedUniformNames(const std::string& source)
    {
        std::set<std::string> names;
        const std::regex quoted("\"(u[A-Za-z0-9]+)\"");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), quoted); it != std::sregex_iterator(); ++it)
        {
            names.insert((*it)[1].str());
        }
        return names;
    }

    bool InTable(const Vk::BuiltInShader shader, const std::string& name)
    {
        if (Vk::FindMaterialUniform(shader, name) != nullptr)
        {
            return true;
        }
        for (const Vk::Uniform& uniform : Vk::LightingUniforms(shader))
        {
            if (uniform.name == name)
            {
                return true;
            }
        }
        return false;
    }

    struct ShaderFiles
    {
        Vk::BuiltInShader shader;
        const char* vertex;
        const char* fragment;
        const char* label;
    };

    constexpr ShaderFiles AllShaders[] = {
        {Vk::BuiltInShader::Unlit, "shaders/vulkan/unlit.vert", "shaders/vulkan/unlit.frag", "unlit"},
        {Vk::BuiltInShader::Lit, "shaders/vulkan/lit.vert", "shaders/vulkan/lit.frag", "lit"},
        {Vk::BuiltInShader::Text, "shaders/vulkan/text.vert", "shaders/vulkan/text.frag", "text"},
    };
}

TEST(VulkanShaderParityTest, TheSourcesAreFound)
{
    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl vertex = ParseGlsl(files.vertex);
        const Glsl fragment = ParseGlsl(files.fragment);
        ASSERT_FALSE(vertex.text.empty()) << files.vertex << " (N2_RENDERER_SOURCE_DIR is " << N2_RENDERER_SOURCE_DIR << ")";
        ASSERT_FALSE(fragment.text.empty()) << files.fragment;
        EXPECT_TRUE(Contains(vertex.text, "#version 450")) << files.vertex;
        EXPECT_TRUE(Contains(fragment.text, "#version 450")) << files.fragment;
    }
    EXPECT_FALSE(ReadFile("src/opengl/OpenGLRenderer.cpp").empty());
}

TEST(VulkanShaderParityTest, EveryBlockDeclaresTheTablesMembersAtTheStd140OffsetsTheTableGives)
{
    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl vertex = ParseGlsl(files.vertex);
        const Glsl fragment = ParseGlsl(files.fragment);

        ExpectBlockMatchesTable(vertex, "PerDraw", 0, 0, Vk::kPerDraw, std::string(files.label) + " vertex");
        const UniformBlock* perDraw = vertex.Block("PerDraw");
        ASSERT_NE(perDraw, nullptr);
        EXPECT_NE(perDraw->layout.find("row_major"), std::string::npos)
            << files.label << ": the matrices are copied row-major, as the engine passes them, so the block says row_major";

        ExpectBlockMatchesTable(fragment, "Material", 1, 0, Vk::MaterialUniforms(files.shader),
                                std::string(files.label) + " fragment");

        if (files.shader == Vk::BuiltInShader::Lit)
        {
            ExpectBlockMatchesTable(fragment, "Lighting", 0, 1, Vk::LightingUniforms(files.shader), "lit fragment");
        }
        else
        {
            EXPECT_EQ(fragment.Block("Lighting"), nullptr) << files.label << " has no lighting block";
        }
        // Nothing else is a block: every non-opaque uniform is in one of them
        EXPECT_EQ(fragment.blocks.size(), files.shader == Vk::BuiltInShader::Lit ? 2u : 1u) << files.label;
        EXPECT_EQ(vertex.blocks.size(), 1u) << files.label;
    }
}

TEST(VulkanShaderParityTest, TheFixedBlockSizesAreTheComputedOnes)
{
    const Glsl lit = ParseGlsl("shaders/vulkan/lit.frag");
    std::uint32_t end = 0;
    bool known = false;
    ASSERT_NE(lit.Block("Lighting"), nullptr);
    PlaceBlock(*lit.Block("Lighting"), lit, end, known);
    ASSERT_TRUE(known);
    EXPECT_EQ(end, Vk::kLightingBlockSize);

    const Glsl vertex = ParseGlsl("shaders/vulkan/unlit.vert");
    ASSERT_NE(vertex.Block("PerDraw"), nullptr);
    PlaceBlock(*vertex.Block("PerDraw"), vertex, end, known);
    EXPECT_EQ(end, Vk::kPerDrawBlockSize);

    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl fragment = ParseGlsl(files.fragment);
        ASSERT_NE(fragment.Block("Material"), nullptr) << files.label;
        PlaceBlock(*fragment.Block("Material"), fragment, end, known);
        ASSERT_TRUE(known) << files.label;
        EXPECT_EQ(end, Vk::MaterialBlockSize(files.shader)) << files.label;
        EXPECT_LE(end, Vk::kMaterialBlockRange) << files.label << ": one descriptor range covers every material block";
    }

    // The light arrays are the ones SceneLighting.hpp sizes
    EXPECT_EQ(Vk::kDirectionalLightCount, 2u);
    EXPECT_EQ(Vk::kPointLightCount, 8u);
    EXPECT_EQ(Vk::kSpotLightCount, 4u);
}

TEST(VulkanShaderParityTest, EveryUniformAMaterialSetsIsInTheTableAndDeclaredByTheShaderGpuCacheWouldPick)
{
    // GpuCache picks the lit shader for Lit and Pbr and the unlit shader for Unlit
    const Glsl lit = ParseGlsl("shaders/vulkan/lit.frag");
    const Glsl unlit = ParseGlsl("shaders/vulkan/unlit.frag");

    const auto texture = Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4});
    for (const ShadingModel shading : {ShadingModel::Lit, ShadingModel::Pbr})
    {
        const auto material = Material::Create(shading);
        material->SetBaseColorTexture(texture);
        RecordingMaterial recorded;
        material->ApplyUniforms(recorded);
        ASSERT_GT(recorded.names.size(), 10u);
        for (const std::string& name : recorded.names)
        {
            EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, name), nullptr)
                << "the lit table has no entry for " << name;
            EXPECT_TRUE(BlockDeclares(lit, "Material", name)) << "the lit Material block does not declare " << name;
        }
    }

    const auto unlitMaterial = Material::Create(ShadingModel::Unlit);
    RecordingMaterial recorded;
    unlitMaterial->ApplyUniforms(recorded);
    ASSERT_FALSE(recorded.names.empty());
    for (const std::string& name : recorded.names)
    {
        EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Unlit, name), nullptr)
            << "the unlit table has no entry for " << name;
        EXPECT_TRUE(BlockDeclares(unlit, "Material", name)) << "the unlit Material block does not declare " << name;
    }
    EXPECT_FALSE(recorded.names.contains("uPbr")) << "an unlit material sets no lit-shader uniform";
}

TEST(VulkanShaderParityTest, EveryDefaultTheOpenGlRendererGivesAMaterialAndEveryUniformTheGlShadersDeclareIsInTheTables)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string material = ReadFile("src/opengl/OpenGLMaterial.cpp");
    ASSERT_FALSE(renderer.empty());

    // OpenGLRenderer::CreateMaterial's per-shader defaults: one region per branch
    const std::size_t from = renderer.find("OpenGLRenderer::CreateMaterial(Common::IShader *shader, Common::ITexture *texture)");
    ASSERT_NE(from, std::string::npos);
    const std::size_t lit = renderer.find("shader == m_standardLitShader", from);
    const std::size_t unlit = renderer.find("shader == m_standardUnlitShader", from);
    const std::size_t text = renderer.find("shader == m_standardTextShader", from);
    const std::size_t end = renderer.find("m_materials.try_emplace", from);
    ASSERT_NE(lit, std::string::npos);
    ASSERT_NE(unlit, std::string::npos);
    ASSERT_NE(text, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    ASSERT_LT(lit, unlit);
    ASSERT_LT(unlit, text);
    ASSERT_LT(text, end);

    const std::set<std::string> litDefaults = QuotedUniformNames(renderer.substr(lit, unlit - lit));
    const std::set<std::string> unlitDefaults = QuotedUniformNames(renderer.substr(unlit, text - unlit));
    const std::set<std::string> textDefaults = QuotedUniformNames(renderer.substr(text, end - text));
    ASSERT_GE(litDefaults.size(), 10u);
    ASSERT_GE(unlitDefaults.size(), 2u);
    ASSERT_GE(textDefaults.size(), 6u);
    for (const std::string& name : litDefaults)
    {
        EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, name), nullptr) << "lit default " << name;
    }
    for (const std::string& name : unlitDefaults)
    {
        EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Unlit, name), nullptr) << "unlit default " << name;
    }
    for (const std::string& name : textDefaults)
    {
        EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Text, name), nullptr) << "text default " << name;
    }
    // The material's own constructor sets the colour every shader has
    EXPECT_TRUE(QuotedUniformNames(material).contains("uAlbedo"));
    for (const ShaderFiles& files : AllShaders)
    {
        EXPECT_NE(Vk::FindMaterialUniform(files.shader, "uAlbedo"), nullptr) << files.label;
    }

    // Every uniform the three GL fragment shaders declare is in the Vulkan interface of the same shader
    const struct
    {
        const char* glName;
        Vk::BuiltInShader shader;
    } glShaders[] = {{"unlitFrag", Vk::BuiltInShader::Unlit},
                     {"litFrag", Vk::BuiltInShader::Lit},
                     {"textFrag", Vk::BuiltInShader::Text}};
    for (const auto& gl : glShaders)
    {
        const std::string source = GlShaderSource(renderer, gl.glName);
        ASSERT_FALSE(source.empty()) << gl.glName;
        const std::set<std::string> names = GlUniformNames(source);
        ASSERT_GE(names.size(), 3u) << gl.glName;
        for (const std::string& name : names)
        {
            EXPECT_TRUE(InTable(gl.shader, name)) << gl.glName << " declares " << name << ", which the Vulkan table lacks";
        }
    }
}

TEST(VulkanShaderParityTest, TheTextShaderTakesTheEffectUniformsInItsMaterialBlock)
{
    const Glsl text = ParseGlsl("shaders/vulkan/text.frag");
    for (const char* name : {"uAlbedo", "uOutline", "uOutlineColor", "uSoftness", "uShadowColor", "uShadowOffset",
                             "uShadowSoftness"})
    {
        EXPECT_TRUE(BlockDeclares(text, "Material", name)) << name;
        EXPECT_NE(Vk::FindMaterialUniform(Vk::BuiltInShader::Text, name), nullptr) << name;
    }
}

TEST(VulkanShaderParityTest, TheSamplersAreBindingsOneToFiveInAuxTextureOrderAfterTheBaseTexture)
{
    // The enum order the binding formula (1 + 1 + value for an AuxTexture) depends on
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Emissive) == 0);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Occlusion) == 1);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Normal) == 2);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::MetallicRoughness) == 3);
    static_assert(Vk::kSamplerCount == 5 && Vk::kFirstSamplerBinding == 1);

    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl fragment = ParseGlsl(files.fragment);
        const std::span<const Vk::Sampler> expected = Vk::Samplers(files.shader);
        ASSERT_EQ(fragment.samplers.size(), expected.size()) << files.label;
        for (const Vk::Sampler& sampler : expected)
        {
            const auto it = fragment.samplers.find(std::string(sampler.name));
            ASSERT_NE(it, fragment.samplers.end()) << files.label << ": " << sampler.name;
            EXPECT_EQ(it->second.set, 1u) << sampler.name;
            EXPECT_EQ(it->second.binding, sampler.binding) << sampler.name;
        }
    }

    const Glsl lit = ParseGlsl("shaders/vulkan/lit.frag");
    EXPECT_EQ(lit.samplers.at("uTexture").binding, 1u);
    EXPECT_EQ(lit.samplers.at("uEmissiveTexture").binding, 2u) << "AuxTexture::Emissive";
    EXPECT_EQ(lit.samplers.at("uOcclusionTexture").binding, 3u) << "AuxTexture::Occlusion";
    EXPECT_EQ(lit.samplers.at("uNormalTexture").binding, 4u) << "AuxTexture::Normal";
    EXPECT_EQ(lit.samplers.at("uMetallicRoughnessTexture").binding, 5u) << "AuxTexture::MetallicRoughness";
}

TEST(VulkanShaderParityTest, TheVertexAttributesAreWhereTheTableAndCommonVertexPutThem)
{
    // The table's offsets are Common::Vertex's (also static_asserted in the header)
    EXPECT_EQ(Vk::kPosition.offset, offsetof(Renderer::Common::Vertex, position));
    EXPECT_EQ(Vk::kNormal.offset, offsetof(Renderer::Common::Vertex, normal));
    EXPECT_EQ(Vk::kTexCoord.offset, offsetof(Renderer::Common::Vertex, texCoord));
    EXPECT_EQ(Vk::kColor.offset, offsetof(Renderer::Common::Vertex, color));
    EXPECT_EQ(Vk::kTangent.offset, offsetof(Renderer::Common::Vertex, tangent));

    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl vertex = ParseGlsl(files.vertex);
        const std::span<const Vk::Attribute> attributes = Vk::Attributes(files.shader);
        std::size_t inputs = 0;
        for (const StageVariable& variable : vertex.variables)
        {
            inputs += variable.direction == "in" ? 1 : 0;
        }
        EXPECT_EQ(inputs, attributes.size()) << files.label << ": the shader reads exactly the attributes the pipeline gives";
        for (const Vk::Attribute& attribute : attributes)
        {
            const StageVariable* variable = vertex.Variable("in", attribute.location);
            ASSERT_NE(variable, nullptr) << files.label << ": location " << attribute.location;
            EXPECT_EQ(variable->name, std::string(attribute.name)) << files.label;
            EXPECT_EQ(ComponentsOf(variable->type), attribute.components) << files.label << ": " << attribute.name;
        }
    }

    // The same names are at the same locations in the OpenGL shaders (the layout the mesh sets up for GL)
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string litVert = GlShaderSource(renderer, "litVert");
    ASSERT_FALSE(litVert.empty());
    for (const Vk::Attribute& attribute : Vk::kLitAttributes)
    {
        const std::string declaration = "layout (location = " + std::to_string(attribute.location) + ") in vec" +
            std::to_string(attribute.components) + " " + std::string(attribute.name) + ";";
        EXPECT_TRUE(Contains(litVert, declaration)) << declaration;
    }
}

TEST(VulkanShaderParityTest, WhatTheVertexStageOutputsIsWhatTheFragmentStageTakes)
{
    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl vertex = ParseGlsl(files.vertex);
        const Glsl fragment = ParseGlsl(files.fragment);
        std::size_t fragmentInputs = 0;
        for (const StageVariable& in : fragment.variables)
        {
            if (in.direction != "in")
            {
                continue;
            }
            ++fragmentInputs;
            const StageVariable* out = vertex.Variable("out", in.location);
            ASSERT_NE(out, nullptr) << files.label << ": nothing outputs location " << in.location;
            EXPECT_EQ(out->type, in.type) << files.label << ": location " << in.location;
            EXPECT_EQ(out->name, in.name) << files.label << ": location " << in.location;
        }
        std::size_t vertexOutputs = 0;
        for (const StageVariable& out : vertex.variables)
        {
            vertexOutputs += out.direction == "out" ? 1 : 0;
        }
        EXPECT_EQ(vertexOutputs, fragmentInputs) << files.label << ": the fragment stage reads everything the vertex stage writes";
        EXPECT_NE(fragment.Variable("out", 0), nullptr) << files.label << ": the colour output";
    }
}

TEST(VulkanShaderParityTest, EveryVertexShaderRemapsClipSpaceZFromGlToVulkan)
{
    for (const ShaderFiles& files : AllShaders)
    {
        const Glsl vertex = ParseGlsl(files.vertex);
        EXPECT_TRUE(Contains(vertex.text, "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;")) << files.label;
        EXPECT_TRUE(Contains(vertex.text, "uProjection * uView")) << files.label << ": the matrices multiply as in GL";
    }
}

TEST(VulkanShaderParityTest, ThePbrConstantsAreTheSoftwareRenderersAndPbrOnlyInputsAreGatedOnUPbr)
{
    const Glsl lit = ParseGlsl("shaders/vulkan/lit.frag");
    EXPECT_NEAR(ConstantIn(lit.text, "MIN_PERCEPTUAL_ROUGHNESS"), Renderer::Common::kMinPerceptualRoughness, 1e-6);
    EXPECT_NEAR(ConstantIn(lit.text, "DIELECTRIC_F0"), Renderer::Common::kDielectricF0, 1e-6);
    EXPECT_NEAR(ConstantIn(lit.text, "PI"), Renderer::Common::kPi, 1e-6);
    // The environment BRDF fit's coefficients
    for (const char* coefficient : {"vec4(-1.0, -0.0275, -0.572, 0.022)", "vec4(1.0, 0.0425, 1.04, -0.04)",
                                    "exp2(-9.28 * nDotV)", "vec2(-1.04, 1.04)"})
    {
        EXPECT_TRUE(Contains(lit.text, coefficient)) << coefficient;
    }
    // A Lit material never samples the metallic-roughness texture
    EXPECT_TRUE(Contains(lit.text, "if (uPbr != 0 && uHasMetallicRoughnessTexture != 0)"));
    // The GGX terms and the sRGB encode are the GL shader's
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string glLit = GlShaderSource(renderer, "litFrag");
    ASSERT_FALSE(glLit.empty());
    for (const char* function : {"float ggxDistribution(float nDotH, float alpha) {", "float smithGgxVisibility(float nDotL, float nDotV, float alpha) {",
                                 "float schlickWeight(float vDotH) {", "float calculateAttenuation(float distance, float range, float attenuation) {",
                                 "vec3 linearToSrgb(vec3 c) {"})
    {
        EXPECT_TRUE(Contains(glLit, function)) << "GL: " << function;
        EXPECT_TRUE(Contains(lit.text, function)) << "Vulkan: " << function;
    }
    // The lighting loops (counts from the lighting block, as GL's uniforms)
    for (const char* loop : {"for (int i = 0; i < uNumDirectionalLights; i++)", "for (int i = 0; i < uNumPointLights; i++)",
                             "for (int i = 0; i < uNumSpotLights; i++)", "float shininess = mix(4.0, 256.0, uSmoothness);"})
    {
        EXPECT_TRUE(Contains(lit.text, loop)) << loop;
    }
}

TEST(VulkanShaderParityTest, TheTextPortKeepsTheAntialiasedSdfMaths)
{
    const Glsl text = ParseGlsl("shaders/vulkan/text.frag");
    for (const char* line : {"float sdf = texture(uTexture, fragTexCoord).r;",
                             "float edgeWidth = max(max(0.5 * fwidth(sdf), 1e-4), uSoftness);",
                             "float coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf);",
                             "vec4 color = uAlbedo * fragColor;", "float alpha = color.a * coverage;",
                             "if (uOutline > 0.0) {", "float outlineEdge = 0.5 - uOutline;",
                             "if (uShadowColor.a > 0.0) {", "texture(uTexture, fragTexCoord - uShadowOffset).r;",
                             "if (result.a < 0.01) {", "discard;"})
    {
        EXPECT_TRUE(Contains(text.text, line)) << line;
    }
    // Both ports are copies of the GL shader, line for line (the GL one is the reference)
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string glText = GlShaderSource(renderer, "textFrag");
    ASSERT_FALSE(glText.empty());
    for (const char* line : {"float sdf = texture(uTexture, fragTexCoord).r;", "float coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf);",
                             "if (result.a < 0.01) {"})
    {
        EXPECT_TRUE(Contains(glText, line)) << "GL: " << line;
    }
}

TEST(VulkanShaderParityTest, TheUnlitPortIsTheGlShadersMaths)
{
    const Glsl unlit = ParseGlsl("shaders/vulkan/unlit.frag");
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string glUnlit = GlShaderSource(renderer, "unlitFrag");
    ASSERT_FALSE(glUnlit.empty());
    for (const char* line : {"vec4 color = uAlbedo;", "color *= texture(uTexture, fragTexCoord);", "color *= fragColor;",
                             "if (color.a < uAlphaCutoff) {", "discard;"})
    {
        EXPECT_TRUE(Contains(glUnlit, line)) << "GL: " << line;
        EXPECT_TRUE(Contains(unlit.text, line)) << "Vulkan: " << line;
    }
    EXPECT_TRUE(Contains(unlit.text, "if (uHasTexture != 0) {")) << "a bool is an int in a block";
}

// ====================================================================================================================
// Stage C: the lighting block member by member (the light structs and their strides), what OpenGL's UpdateSceneLighting
// sets, and the defaults OpenGL's CreateMaterial gives a lit material
// ====================================================================================================================

namespace
{
    struct StructTable
    {
        const char* glslName;  // the struct's name in lit.frag
        const char* arrayName; // the lighting block member that is an array of it
        std::span<const Vk::Member> members;
        std::uint32_t stride;
        std::uint32_t count;
    };

    std::vector<StructTable> LightStructTables()
    {
        return {
            {"DirectionalLight", "uDirectionalLights", Vk::kDirectionalLightMembers, Vk::kDirectionalLightStride,
             Vk::kDirectionalLightCount},
            {"PointLight", "uPointLights", Vk::kPointLightMembers, Vk::kPointLightStride, Vk::kPointLightCount},
            {"SpotLight", "uSpotLights", Vk::kSpotLightMembers, Vk::kSpotLightStride, Vk::kSpotLightCount},
        };
    }

    /// The members of a GLSL struct at their std140 offsets from the start of the struct (each aligned to its own
    /// alignment: a vec3 to 16, with a float allowed in the 4 bytes after it)
    std::vector<PlacedMember> PlaceStruct(const std::vector<Member>& members, const Glsl& glsl, std::uint32_t& size,
                                          bool& known)
    {
        std::vector<PlacedMember> placed;
        std::uint32_t cursor = 0;
        known = true;
        for (const Member& member : members)
        {
            const Layout layout = Std140Layout(member.type, member.arrayCount, glsl);
            if (!layout.known)
            {
                known = false;
                break;
            }
            PlacedMember entry;
            entry.name = member.name;
            entry.type = member.type;
            entry.arrayCount = member.arrayCount;
            entry.offset = AlignUp(cursor, layout.alignment);
            entry.size = layout.size;
            cursor = entry.offset + entry.size;
            placed.push_back(entry);
        }
        size = AlignUp(cursor, 16); // a std140 struct is a multiple of 16 bytes
        return placed;
    }

    /// The part of `source` from the text `from` up to (not including) the text `to`
    std::string Region(const std::string& source, const std::string& from, const std::string& to)
    {
        const std::size_t start = source.find(from);
        if (start == std::string::npos)
        {
            return {};
        }
        const std::size_t end = source.find(to, start + from.size());
        return end == std::string::npos ? std::string{} : source.substr(start, end - start);
    }

    Vk::UniformType TypeOfSetter(const std::string& setter)
    {
        if (setter == "Int") return Vk::UniformType::Int;
        if (setter == "Float") return Vk::UniformType::Float;
        if (setter == "Vec2") return Vk::UniformType::Vec2;
        if (setter == "Vec3") return Vk::UniformType::Vec3;
        return Vk::UniformType::Vec4;
    }

    const Vk::Uniform* FindLighting(const std::string& name)
    {
        for (const Vk::Uniform& uniform : Vk::kLitLighting)
        {
            if (uniform.name == name)
            {
                return &uniform;
            }
        }
        return nullptr;
    }

    std::vector<float> NumbersIn(const std::string& arguments)
    {
        std::vector<float> numbers;
        const std::regex number(R"(-?\d+(?:\.\d+)?f?)");
        for (auto it = std::sregex_iterator(arguments.begin(), arguments.end(), number); it != std::sregex_iterator(); ++it)
        {
            numbers.push_back(std::stof((*it)[0].str()));
        }
        return numbers;
    }

    std::size_t ComponentCount(const Vk::UniformType type)
    {
        switch (type)
        {
        case Vk::UniformType::Vec2: return 2;
        case Vk::UniformType::Vec3: return 3;
        case Vk::UniformType::Vec4: return 4;
        default: return 1;
        }
    }
}

TEST(VulkanShaderParityTest, TheLightStructsAreCheckedMemberByMemberAgainstTheGlslAtTheirStd140OffsetsAndStrides)
{
    const Glsl lit = ParseGlsl("shaders/vulkan/lit.frag");
    const UniformBlock* lighting = lit.Block("Lighting");
    ASSERT_NE(lighting, nullptr);

    for (const StructTable& table : LightStructTables())
    {
        const auto it = lit.structs.find(table.glslName);
        ASSERT_NE(it, lit.structs.end()) << table.glslName << " is declared in lit.frag";

        std::uint32_t size = 0;
        bool known = false;
        const std::vector<PlacedMember> placed = PlaceStruct(it->second, lit, size, known);
        ASSERT_TRUE(known) << table.glslName;
        ASSERT_EQ(placed.size(), table.members.size()) << table.glslName << " has another number of members";
        for (std::size_t i = 0; i < table.members.size(); ++i)
        {
            EXPECT_EQ(placed[i].name, std::string(table.members[i].name)) << table.glslName << " member " << i;
            EXPECT_EQ(placed[i].offset, table.members[i].offset) << table.glslName << "." << table.members[i].name;
            EXPECT_EQ(placed[i].type, std::string(GlslTypeOf(table.members[i].type)))
                << table.glslName << "." << table.members[i].name;
        }
        // The array stride is the struct's size rounded to 16, and the table's stride and count are the block's array
        EXPECT_EQ(size, table.stride) << table.glslName << " is " << table.stride << " bytes in std140";

        const Vk::Uniform* array = FindLighting(table.arrayName);
        ASSERT_NE(array, nullptr) << table.arrayName;
        EXPECT_EQ(array->type, Vk::UniformType::Struct);
        EXPECT_EQ(array->arrayCount, table.count) << table.arrayName;
        EXPECT_EQ(array->size, table.stride * table.count) << table.arrayName << ": count x stride";
        const auto declared = std::find_if(lighting->members.begin(), lighting->members.end(),
                                           [&](const Member& member) { return member.name == table.arrayName; });
        ASSERT_NE(declared, lighting->members.end()) << table.arrayName;
        EXPECT_EQ(declared->type, std::string(table.glslName)) << table.arrayName << " is an array of that struct";
        EXPECT_EQ(declared->arrayCount, table.count) << table.arrayName << "'s size in the GLSL";
    }
    std::uint32_t end = 0;
    bool known = false;
    PlaceBlock(*lighting, lit, end, known);
    EXPECT_EQ(end, Vk::kLightingBlockSize);
}

TEST(VulkanShaderParityTest, EveryNameOpenGlsUpdateSceneLightingSetsHasAMatchingEntryOfTheRightTypeAndNoEntryIsLeftUnset)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string region =
        Region(renderer, "void OpenGLRenderer::UpdateSceneLighting(", "void OpenGLRenderer::ApplyRenderState");
    ASSERT_FALSE(region.empty());
    const std::string source = StripComments(region);

    // Every shader->SetXxx("name", ...) or SetXxx(base + ".member", ...) in order, with the array name `base` seen last
    const std::regex setting(
        R"re(base\s*=\s*"(u[A-Za-z]+)\["|shader->Set(Int|Float|Vec2|Vec3|Vec4)\(\s*(base\s*\+\s*)?"([^"]+)")re");
    std::map<std::string, std::set<std::string>> structMembersSet;
    std::set<std::string> looseSet;
    std::string base;
    const std::vector<StructTable> tables = LightStructTables();
    for (auto it = std::sregex_iterator(source.begin(), source.end(), setting); it != std::sregex_iterator(); ++it)
    {
        const std::smatch& match = *it;
        if (match[1].matched)
        {
            base = match[1].str();
            continue;
        }
        const Vk::UniformType type = TypeOfSetter(match[2].str());
        const std::string name = match[4].str();
        if (match[3].matched)
        {
            ASSERT_FALSE(base.empty()) << "a struct member is set before any array name: " << name;
            ASSERT_EQ(name.front(), '.') << name;
            const std::string member = name.substr(1);
            const StructTable* found = nullptr;
            for (const StructTable& table : tables)
            {
                if (base == table.arrayName)
                {
                    found = &table;
                }
            }
            ASSERT_NE(found, nullptr) << base << " is not a light array of the lighting table";
            const auto entry = std::find_if(found->members.begin(), found->members.end(),
                                            [&](const Vk::Member& candidate) { return candidate.name == member; });
            ASSERT_NE(entry, found->members.end()) << "GL sets " << base << "[i]." << member << ": no table entry";
            EXPECT_EQ(entry->type, type) << base << "." << member << ": GL's setter and the table's type agree";
            structMembersSet[base].insert(member);
        }
        else
        {
            const Vk::Uniform* entry = FindLighting(name);
            ASSERT_NE(entry, nullptr) << "GL sets " << name << ": the lighting table has no entry";
            EXPECT_EQ(entry->type, type) << name << ": GL's setter and the table's type agree";
            looseSet.insert(name);
        }
    }

    // Nothing the table has is left out of what GL sets: the loose members and every member of every light struct
    for (const Vk::Uniform& uniform : Vk::kLitLighting)
    {
        if (uniform.type != Vk::UniformType::Struct)
        {
            EXPECT_TRUE(looseSet.contains(std::string(uniform.name))) << "GL never sets " << uniform.name;
        }
    }
    for (const StructTable& table : tables)
    {
        for (const Vk::Member& member : table.members)
        {
            EXPECT_TRUE(structMembersSet[table.arrayName].contains(std::string(member.name)))
                << "GL never sets " << table.arrayName << "[i]." << member.name;
        }
    }
    EXPECT_GE(looseSet.size(), 6u);
}

TEST(VulkanShaderParityTest, EveryDefaultTheOpenGlRendererGivesALitMaterialIsInTheTableWithTheSameValueAndType)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string region = StripComments(
        Region(renderer, "if (shader == m_standardLitShader)", "else if (shader == m_standardUnlitShader)"));
    ASSERT_FALSE(region.empty());

    std::set<std::string> seen;
    const std::regex setting(R"re(material->Set(Int|Float|Vec2|Vec3|Vec4)\(\s*"(u[A-Za-z]+)"\s*,([^;]*)\);)re");
    for (auto it = std::sregex_iterator(region.begin(), region.end(), setting); it != std::sregex_iterator(); ++it)
    {
        const Vk::UniformType type = TypeOfSetter((*it)[1].str());
        const std::string name = (*it)[2].str();
        const std::string arguments = (*it)[3].str();
        const Vk::Uniform* entry = Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, name);
        ASSERT_NE(entry, nullptr) << "the lit table has no entry for the default " << name;
        EXPECT_EQ(entry->type, type) << name << ": GL's setter and the table's type agree";
        seen.insert(name);
        if (name == "uHasTexture")
        {
            continue; // depends on the texture given at creation (written by the VulkanMaterial constructor)
        }
        const std::span<const Vk::MaterialDefault> defaults = Vk::kLitMaterialDefaults;
        const auto entryDefault = std::find_if(defaults.begin(), defaults.end(),
                                               [&](const Vk::MaterialDefault& candidate) { return candidate.name == name; });
        ASSERT_NE(entryDefault, defaults.end()) << "no default value in the table for " << name;
        const std::vector<float> numbers = NumbersIn(arguments);
        const std::size_t components = ComponentCount(type);
        ASSERT_EQ(numbers.size(), components) << name << ": " << arguments;
        for (std::size_t i = 0; i < components; ++i)
        {
            EXPECT_FLOAT_EQ(entryDefault->values[i], numbers[i]) << name << " component " << i << " (GL's default)";
        }
    }
    // The material's own constructor gives uAlbedo white
    EXPECT_NE(ReadFile("src/opengl/OpenGLMaterial.cpp").find("SetColor(\"uAlbedo\", 1, 1, 1, 1)"), std::string::npos);

    // The table's defaults are exactly GL's, plus uAlbedo (OpenGLMaterial's constructor)
    for (const Vk::MaterialDefault& entry : Vk::kLitMaterialDefaults)
    {
        if (entry.name == "uAlbedo")
        {
            for (const float value : entry.values)
            {
                EXPECT_FLOAT_EQ(value, 1.0f);
            }
            continue;
        }
        EXPECT_TRUE(seen.contains(std::string(entry.name))) << "GL's CreateMaterial does not set " << entry.name;
    }
    // Every lit material member has a default: from the table, or uHasTexture from the texture given at creation
    for (const Vk::Uniform& uniform : Vk::kLitMaterial)
    {
        EXPECT_TRUE(seen.contains(std::string(uniform.name)) || uniform.name == "uAlbedo")
            << "GL gives " << uniform.name << " no default";
    }
}

TEST(VulkanShaderParityTest, EveryDefaultTheOpenGlRendererGivesAnUnlitMaterialIsInTheTableWithTheSameValueAndType)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string region = StripComments(
        Region(renderer, "else if (shader == m_standardUnlitShader)", "else if (shader == m_standardTextShader)"));
    ASSERT_FALSE(region.empty());
    std::set<std::string> seen;
    const std::regex setting(R"re(material->Set(Int|Float|Vec2|Vec3|Vec4)\(\s*"(u[A-Za-z]+)"\s*,([^;]*)\);)re");
    for (auto it = std::sregex_iterator(region.begin(), region.end(), setting); it != std::sregex_iterator(); ++it)
    {
        const std::string name = (*it)[2].str();
        const Vk::Uniform* entry = Vk::FindMaterialUniform(Vk::BuiltInShader::Unlit, name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->type, TypeOfSetter((*it)[1].str())) << name;
        seen.insert(name);
        if (name == "uAlphaCutoff")
        {
            const std::vector<float> numbers = NumbersIn((*it)[3].str());
            ASSERT_EQ(numbers.size(), 1u);
            EXPECT_FLOAT_EQ(numbers[0], 0.0f);
        }
    }
    EXPECT_TRUE(seen.contains("uHasTexture"));
    EXPECT_TRUE(seen.contains("uAlphaCutoff"));
    for (const Vk::MaterialDefault& entry : Vk::kUnlitMaterialDefaults)
    {
        EXPECT_TRUE(entry.name == "uAlbedo" || seen.contains(std::string(entry.name))) << entry.name;
    }
}

// ====================================================================================================================
// The text shader (stage E of #43): what OpenGL gives a text material, what the engine's text drawing sets, and the maths
// ====================================================================================================================

TEST(VulkanShaderParityTest, EveryDefaultTheOpenGlRendererGivesATextMaterialIsInTheTableWithTheSameValueAndType)
{
    const std::string renderer = ReadFile("src/opengl/OpenGLRenderer.cpp");
    const std::string region =
        StripComments(Region(renderer, "else if (shader == m_standardTextShader)", "m_materials.try_emplace"));
    ASSERT_FALSE(region.empty());

    std::set<std::string> seen;
    const std::regex setting(R"re(material->Set(Int|Float|Vec2|Vec3|Vec4)\(\s*"(u[A-Za-z]+)"\s*,([^;]*)\);)re");
    for (auto it = std::sregex_iterator(region.begin(), region.end(), setting); it != std::sregex_iterator(); ++it)
    {
        const Vk::UniformType type = TypeOfSetter((*it)[1].str());
        const std::string name = (*it)[2].str();
        const Vk::Uniform* entry = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, name);
        ASSERT_NE(entry, nullptr) << "the text table has no entry for the default " << name;
        EXPECT_EQ(entry->type, type) << name << ": GL's setter and the table's type agree";
        seen.insert(name);

        const std::span<const Vk::MaterialDefault> defaults = Vk::kTextMaterialDefaults;
        const auto entryDefault = std::find_if(defaults.begin(), defaults.end(),
                                               [&](const Vk::MaterialDefault& candidate) { return candidate.name == name; });
        ASSERT_NE(entryDefault, defaults.end()) << "no default value in the table for " << name;
        const std::vector<float> numbers = NumbersIn((*it)[3].str());
        const std::size_t components = ComponentCount(type);
        ASSERT_EQ(numbers.size(), components) << name << ": " << (*it)[3].str();
        for (std::size_t i = 0; i < components; ++i)
        {
            EXPECT_FLOAT_EQ(entryDefault->values[i], numbers[i]) << name << " component " << i << " (GL's default)";
        }
    }
    // The six effect uniforms, every one of them, as GL sets them for a text material
    for (const char* name : {"uOutline", "uOutlineColor", "uSoftness", "uShadowColor", "uShadowOffset", "uShadowSoftness"})
    {
        EXPECT_TRUE(seen.contains(name)) << "GL's CreateMaterial does not set " << name << " for the text shader";
    }
    // The table's defaults are exactly GL's, plus uAlbedo (OpenGLMaterial's constructor: white)
    for (const Vk::MaterialDefault& entry : Vk::kTextMaterialDefaults)
    {
        if (entry.name == "uAlbedo")
        {
            for (const float value : entry.values)
            {
                EXPECT_FLOAT_EQ(value, 1.0f);
            }
            continue;
        }
        EXPECT_TRUE(seen.contains(std::string(entry.name))) << "GL's CreateMaterial does not set " << entry.name;
    }
    // Every text material member has a default
    for (const Vk::Uniform& uniform : Vk::kTextMaterial)
    {
        EXPECT_TRUE(seen.contains(std::string(uniform.name)) || uniform.name == "uAlbedo")
            << "GL gives " << uniform.name << " no default";
    }
    EXPECT_EQ(Vk::kTextMaterial.size(), Vk::kTextMaterialDefaults.size());
}

TEST(VulkanShaderParityTest, EveryUniformTheEnginesTextDrawingSetsIsInTheTextTableWithTheSameType)
{
    // engine/src/rendering/TextDrawing.cpp sets the text material's values for every draw (the face, and each effect pass)
    const std::string drawing = StripComments(ReadFile("../engine/src/rendering/TextDrawing.cpp"));
    ASSERT_FALSE(drawing.empty()) << "N2_RENDERER_SOURCE_DIR is " << N2_RENDERER_SOURCE_DIR;

    std::map<std::string, Vk::UniformType> set;
    const std::regex setting(R"re(_material->Set(Float|Vec2|Vec3|Vec4|Color|Int)\(\s*"(u[A-Za-z]+)")re");
    for (auto it = std::sregex_iterator(drawing.begin(), drawing.end(), setting); it != std::sregex_iterator(); ++it)
    {
        const std::string setter = (*it)[1].str();
        set[(*it)[2].str()] = setter == "Color" ? Vk::UniformType::Vec4 : TypeOfSetter(setter);
    }
    ASSERT_GE(set.size(), 7u);
    for (const auto& [name, type] : set)
    {
        const Vk::Uniform* entry = Vk::FindMaterialUniform(Vk::BuiltInShader::Text, name);
        ASSERT_NE(entry, nullptr) << "TextDrawing sets " << name << ", which the text table lacks";
        EXPECT_EQ(entry->type, type) << name << ": the engine's setter and the table's type agree";
    }
    for (const char* name : {"uAlbedo", "uOutline", "uOutlineColor", "uSoftness", "uShadowColor", "uShadowOffset",
                             "uShadowSoftness"})
    {
        EXPECT_TRUE(set.contains(name)) << "TextDrawing no longer sets " << name;
    }
    // ...and nothing else: every text table entry is one the engine sets
    for (const Vk::Uniform& uniform : Vk::kTextMaterial)
    {
        EXPECT_TRUE(set.contains(std::string(uniform.name))) << uniform.name << " is in the table but never set by the engine";
    }
}

TEST(VulkanShaderParityTest, TheTextPortIsTheGlShadersMathsLineForLine)
{
    // From the `over` helper to the end of main: the same statements in the same order, comments and layout aside. The
    // declarations above it differ by design (a std140 block with a binding instead of loose uniforms, in/out locations)
    const std::string vulkan = StripComments(ReadFile("shaders/vulkan/text.frag"));
    const std::string gl = StripComments(GlShaderSource(ReadFile("src/opengl/OpenGLRenderer.cpp"), "textFrag"));
    ASSERT_FALSE(vulkan.empty());
    ASSERT_FALSE(gl.empty());
    const auto fromOver = [](const std::string& source)
    {
        const std::size_t at = source.find("vec4 over(");
        return at == std::string::npos ? std::string{} : Normalised(source.substr(at));
    };
    const std::string vulkanMaths = fromOver(vulkan);
    const std::string glMaths = fromOver(gl);
    ASSERT_FALSE(vulkanMaths.empty());
    ASSERT_FALSE(glMaths.empty());
    EXPECT_EQ(vulkanMaths, glMaths) << "text.frag's maths differs from GL's textFrag";

    // The constants the shader depends on, in both: the iso-line 0.5, the minimum ramp 1e-4, the discard below alpha 0.01,
    // the smoothstep bounds, and the `over` order (the face over the outline, that over the shadow)
    for (const char* needle :
         {"0.5 * fwidth(sdf)", "1e-4", "smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf)", "if (result.a < 0.01) {",
          "float outlineEdge = 0.5 - uOutline;",
          "result = over(result, vec4(uOutlineColor.rgb, uOutlineColor.a * outlineCoverage));",
          "result = over(result, vec4(uShadowColor.rgb, uShadowColor.a * shadowCoverage));",
          "smoothstep(max(outlineEdge - edgeWidth, 0.0), outlineEdge + edgeWidth, sdf)",
          "texture(uTexture, fragTexCoord - uShadowOffset).r"})
    {
        EXPECT_TRUE(Contains(vulkan, needle)) << "Vulkan: " << needle;
        EXPECT_TRUE(Contains(gl, needle)) << "GL: " << needle;
    }
    // The outline is composited before the shadow (so the shadow ends up under both)
    ASSERT_NE(vulkanMaths.find("uOutlineColor.rgb"), std::string::npos);
    ASSERT_NE(glMaths.find("uOutlineColor.rgb"), std::string::npos);
    EXPECT_LT(vulkanMaths.find("uOutlineColor.rgb"), vulkanMaths.find("uShadowColor.rgb"));
    EXPECT_LT(glMaths.find("uOutlineColor.rgb"), glMaths.find("uShadowColor.rgb"));
}
