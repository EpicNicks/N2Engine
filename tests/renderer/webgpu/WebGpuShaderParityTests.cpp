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

#include <renderer/common/GpuShaderInterface.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/webgpu/WgslSources.hpp>

#include "engine/rendering/Material.hpp"
#include "engine/rendering/Texture.hpp"

// The parity test of the WebGPU backend's shaders (G1 of #155), the WGSL twin of VulkanShaderParityTests.cpp. It needs NO
// GPU, no WebGPU library and no Vulkan: it reads the WGSL under renderer/shaders/webgpu/ as TEXT, so it runs on every leg that
// runs the renderer tests (the Windows ones, the web one under node, the Vulkan-off ones). For the unlit, lit and text shaders
// it ties together:
//   - every uniform block (the per-draw, lighting and material structs behind the var<uniform> declarations) declares the
//     members of the shared interface table (renderer/common/GpuShaderInterface.hpp) in order, with the table's types, array
//     counts and byte offsets and sizes, which a small WGSL layout calculator (the uniform address space rules: alignments of
//     scalars, vectors, mat4x4, structs and arrays, array strides that are multiples of 16) computes from the declarations;
//   - the resources are exactly the table's: group 0 binding 0 per-draw (binding 1 lighting, lit only), group 1 binding 0
//     material, every table sampler as a texture_2d at the table's binding and a sampler named <name>Sampler at that binding
//     plus kSamplerCount, and nothing else;
//   - the vertex inputs are the table's attributes (location, name, component count), what the vertex stage outputs is what
//     the fragment stage takes, and the fragment stage writes the colour at location 0;
//   - every uniform Material::ApplyUniforms sets, and every material default, is a member of the WGSL material struct;
//   - the maths the port must keep (the clip-space z remap, the matrix product order, the PBR constants and the environment
//     BRDF fit, the SDF text ramp, the unlit alpha test, the 3x3 inverse), and the uniformity rule (every textureSample and
//     fwidth comes before the first discard, and in the lit shader before any lighting loop);
//   - the WGSL embedded by CMake (renderer/webgpu/WgslSources.hpp) is the files' text.
// A renamed or forgotten uniform, a reordered block member, a moved binding or a member whose WGSL offset is not the std140
// one fails here with the file and binding named, instead of drawing wrong colours (an unknown uniform name is otherwise
// silently ignored). Tint and naga checking that the files are valid WGSL is the CI job "wgsl (Tint, naga)" (scripts/validate-wgsl.sh).

using namespace N2Engine;
using Rendering::Material;
using Rendering::ShadingModel;
using Rendering::Texture;
namespace Gpu = Renderer::Gpu::ShaderInterface;

namespace
{
    std::string ReadFile(const std::string& relative)
    {
        std::ifstream file(std::string(N2_RENDERER_SOURCE_DIR) + "/" + relative, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::string WithoutCarriageReturns(const std::string_view text)
    {
        std::string out;
        for (const char c : text)
        {
            if (c != '\r')
            {
                out.push_back(c);
            }
        }
        return out;
    }

    /// Runs of whitespace (newlines, indentation) as one space, none at the ends, so a reformatted shader still matches
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

    std::string RemoveSpaces(const std::string& text)
    {
        std::string out;
        for (const char c : text)
        {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
    {
        std::size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos)
        {
            text.replace(at, from.size(), to);
            at += to.size();
        }
        return text;
    }

    /// A WGSL type written without spaces and with the predeclared aliases (vec3f, mat4x4f) spelled out
    std::string NormalisedType(const std::string& written)
    {
        std::string type = RemoveSpaces(written);
        type = ReplaceAll(type, "vec2f", "vec2<f32>");
        type = ReplaceAll(type, "vec3f", "vec3<f32>");
        type = ReplaceAll(type, "vec4f", "vec4<f32>");
        type = ReplaceAll(type, "mat4x4f", "mat4x4<f32>");
        return type;
    }

    // ---- A very small WGSL reader: structs, module-scope resources, the two entry points ----------------------------
    struct Field
    {
        std::string name;
        std::string type;          // normalised
        int location = -1;         // -1: none
        std::string builtin;       // empty: none
        bool hasAlignOrSize = false;
    };

    struct Resource
    {
        std::uint32_t group = 0;
        std::uint32_t binding = 0;
        std::string space;         // "uniform", or empty for a texture or a sampler
        std::string name;
        std::string type;          // normalised
    };

    struct EntryPoint
    {
        bool found = false;
        std::string name;
        std::string parameterType;
        std::string resultType;
    };

    struct Module
    {
        std::string file;
        std::string text; // comments removed
        std::map<std::string, std::vector<Field>> structs;
        std::vector<Resource> resources;
        EntryPoint vertex;
        EntryPoint fragment;

        [[nodiscard]] const Resource* At(const std::uint32_t group, const std::uint32_t binding) const
        {
            for (const Resource& resource : resources)
            {
                if (resource.group == group && resource.binding == binding)
                {
                    return &resource;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const std::vector<Field>* Struct(const std::string& name) const
        {
            const auto it = structs.find(name);
            return it == structs.end() ? nullptr : &it->second;
        }
    };

    /// `text` split at the commas that are not inside <>, () or []
    std::vector<std::string> SplitTopLevel(const std::string& text)
    {
        std::vector<std::string> pieces;
        std::string current;
        int depth = 0;
        for (const char c : text)
        {
            if (c == '<' || c == '(' || c == '[')
            {
                ++depth;
            }
            else if (c == '>' || c == ')' || c == ']')
            {
                --depth;
            }
            if (c == ',' && depth == 0)
            {
                pieces.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        pieces.push_back(current);
        return pieces;
    }

    std::vector<Field> ParseFields(const std::string& body)
    {
        std::vector<Field> fields;
        const std::regex declaration(R"(^((?:@[A-Za-z_]+(?: ?\([^)]*\))? ?)*)([A-Za-z_][A-Za-z0-9_]*) ?: ?(.+)$)");
        const std::regex location(R"(@location ?\( ?(\d+) ?\))");
        const std::regex builtin(R"(@builtin ?\( ?([a-z_]+) ?\))");
        for (const std::string& piece : SplitTopLevel(body))
        {
            const std::string normalised = Normalised(piece);
            if (normalised.empty())
            {
                continue; // a trailing comma
            }
            std::smatch match;
            if (!std::regex_match(normalised, match, declaration))
            {
                Field unknown;
                unknown.name = "<unparsed: " + normalised + ">";
                fields.push_back(unknown);
                continue;
            }
            Field field;
            const std::string attributes = match[1].str();
            field.name = match[2].str();
            field.type = NormalisedType(match[3].str());
            std::smatch attribute;
            if (std::regex_search(attributes, attribute, location))
            {
                field.location = std::stoi(attribute[1].str());
            }
            if (std::regex_search(attributes, attribute, builtin))
            {
                field.builtin = attribute[1].str();
            }
            field.hasAlignOrSize = attributes.find("@align") != std::string::npos || attributes.find("@size") != std::string::npos;
            fields.push_back(field);
        }
        return fields;
    }

    Module ParseWgsl(const std::string& relative)
    {
        Module module;
        module.file = relative;
        module.text = StripComments(ReadFile(relative));
        const std::string& text = module.text;

        const std::regex structure(R"(struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\})");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), structure); it != std::sregex_iterator(); ++it)
        {
            module.structs[(*it)[1].str()] = ParseFields((*it)[2].str());
        }

        const std::regex resource(
            R"(@group\(\s*(\d+)\s*\)\s*@binding\(\s*(\d+)\s*\)\s*var\s*(?:<\s*([A-Za-z_, ]*)\s*>)?\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^;]+);)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), resource); it != std::sregex_iterator(); ++it)
        {
            Resource parsed;
            parsed.group = static_cast<std::uint32_t>(std::stoul((*it)[1].str()));
            parsed.binding = static_cast<std::uint32_t>(std::stoul((*it)[2].str()));
            parsed.space = RemoveSpaces((*it)[3].str());
            parsed.name = (*it)[4].str();
            parsed.type = NormalisedType((*it)[5].str());
            module.resources.push_back(parsed);
        }

        const std::regex vertex(R"(@vertex\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*->\s*([A-Za-z_][A-Za-z0-9_]*))");
        std::smatch match;
        if (std::regex_search(text, match, vertex))
        {
            module.vertex = {true, match[1].str(), match[2].str(), match[3].str()};
        }
        const std::regex fragment(
            R"(@fragment\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*->\s*@location\(\s*0\s*\)\s*(vec4\s*<\s*f32\s*>))");
        if (std::regex_search(text, match, fragment))
        {
            module.fragment = {true, match[1].str(), match[2].str(), NormalisedType(match[3].str())};
        }
        return module;
    }

    /// The part of `source` from `from` to the end
    std::string From(const std::string& source, const std::string& from)
    {
        const std::size_t start = source.find(from);
        return start == std::string::npos ? std::string{} : source.substr(start);
    }

    // ---- The WGSL layout rules of the uniform address space ---------------------------------------------------------
    struct Layout
    {
        std::uint32_t alignment = 0;
        std::uint32_t size = 0;
        bool known = false;
    };

    struct PlacedMember
    {
        std::string name;
        std::string type;
        std::uint32_t arrayCount = 1;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    std::uint32_t AlignUp(const std::uint32_t value, const std::uint32_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    struct Placement
    {
        std::vector<PlacedMember> members;
        std::uint32_t end = 0;  // where the last member ends
        Layout layout;          // the struct's alignment and size (the size rounded up to the alignment)
        std::vector<std::string> problems;
    };

    Layout LayoutOfType(const Module& module, const std::string& type, std::uint32_t& arrayCount, std::vector<std::string>& problems);

    /// The members of a struct at the offsets WGSL gives them, and what the uniform address space forbids (a structure member
    /// at an offset that is not a multiple of 16, an array whose stride is not)
    Placement PlaceStruct(const Module& module, const std::string& name)
    {
        Placement placement;
        const std::vector<Field>* fields = module.Struct(name);
        if (fields == nullptr)
        {
            placement.problems.push_back("no struct " + name);
            return placement;
        }
        std::uint32_t cursor = 0;
        std::uint32_t structAlignment = 1;
        for (const Field& field : *fields)
        {
            if (field.hasAlignOrSize)
            {
                placement.problems.push_back(name + "." + field.name + " uses @align or @size, which the parity test does not model");
            }
            std::uint32_t arrayCount = 1;
            const Layout layout = LayoutOfType(module, field.type, arrayCount, placement.problems);
            if (!layout.known)
            {
                placement.problems.push_back(name + "." + field.name + " has a type the layout calculator does not know: " + field.type);
                return placement;
            }
            PlacedMember member;
            member.name = field.name;
            member.type = field.type;
            member.arrayCount = arrayCount;
            member.offset = AlignUp(cursor, layout.alignment);
            member.size = layout.size;
            if ((field.type.rfind("array<", 0) == 0 || module.structs.contains(field.type)) && member.offset % 16 != 0)
            {
                placement.problems.push_back(name + "." + field.name + " is a structure or array at offset " +
                                             std::to_string(member.offset) + ", which the uniform address space needs to be a multiple of 16");
            }
            cursor = member.offset + member.size;
            structAlignment = std::max(structAlignment, layout.alignment);
            placement.members.push_back(member);
        }
        placement.end = cursor;
        placement.layout = {structAlignment, AlignUp(cursor, structAlignment), true};
        return placement;
    }

    Layout LayoutOfType(const Module& module, const std::string& type, std::uint32_t& arrayCount, std::vector<std::string>& problems)
    {
        arrayCount = 1;
        if (type == "i32" || type == "u32" || type == "f32")
        {
            return {4, 4, true};
        }
        if (type == "vec2<f32>")
        {
            return {8, 8, true};
        }
        if (type == "vec3<f32>")
        {
            return {16, 12, true};
        }
        if (type == "vec4<f32>")
        {
            return {16, 16, true};
        }
        if (type == "mat4x4<f32>")
        {
            return {16, 64, true}; // four vec4 columns
        }
        std::smatch match;
        if (std::regex_match(type, match, std::regex(R"(array<(.+),(\d+)>)")))
        {
            std::uint32_t ignored = 1;
            const Layout element = LayoutOfType(module, match[1].str(), ignored, problems);
            if (!element.known)
            {
                return {};
            }
            arrayCount = static_cast<std::uint32_t>(std::stoul(match[2].str()));
            const std::uint32_t stride = AlignUp(element.size, element.alignment);
            if (stride % 16 != 0)
            {
                problems.push_back("array " + type + " has stride " + std::to_string(stride) +
                                   ", which the uniform address space needs to be a multiple of 16");
            }
            return {element.alignment, stride * arrayCount, true};
        }
        if (module.structs.contains(type))
        {
            const Placement placement = PlaceStruct(module, type);
            problems.insert(problems.end(), placement.problems.begin(), placement.problems.end());
            return placement.layout;
        }
        return {};
    }

    const char* WgslTypeOf(const Gpu::UniformType type)
    {
        switch (type)
        {
        case Gpu::UniformType::Int: return "i32";
        case Gpu::UniformType::Float: return "f32";
        case Gpu::UniformType::Vec2: return "vec2<f32>";
        case Gpu::UniformType::Vec3: return "vec3<f32>";
        case Gpu::UniformType::Vec4: return "vec4<f32>";
        case Gpu::UniformType::Mat4: return "mat4x4<f32>";
        case Gpu::UniformType::Struct: return "";
        }
        return "";
    }

    std::uint32_t ComponentsOf(const std::string& type)
    {
        if (type == "vec2<f32>") return 2;
        if (type == "vec3<f32>") return 3;
        if (type == "vec4<f32>") return 4;
        if (type == "f32") return 1;
        return 0;
    }

    /// The uniform struct behind the var<uniform> at group/binding, or nullptr after reporting why not
    const Resource* ExpectUniformResource(const Module& module, const std::uint32_t group, const std::uint32_t binding,
                                          const std::string& what)
    {
        const Resource* resource = module.At(group, binding);
        EXPECT_NE(resource, nullptr) << module.file << ": no resource at group " << group << " binding " << binding << " (" << what << ")";
        if (resource == nullptr)
        {
            return nullptr;
        }
        EXPECT_EQ(resource->space, "uniform") << module.file << ": group " << group << " binding " << binding << " (" << what << ") is var<uniform>";
        EXPECT_TRUE(module.structs.contains(resource->type))
            << module.file << ": group " << group << " binding " << binding << " (" << what << ") is a struct, not " << resource->type;
        return module.structs.contains(resource->type) ? resource : nullptr;
    }

    /// Checks one uniform struct against a table: the same members in the same order, with the table's types, array counts,
    /// offsets and sizes, nothing the uniform address space forbids; returns where the last member ends
    std::uint32_t ExpectBlockMatchesTable(const Module& module, const std::uint32_t group, const std::uint32_t binding,
                                          const std::span<const Gpu::Uniform> table, const std::string& what)
    {
        const Resource* resource = ExpectUniformResource(module, group, binding, what);
        if (resource == nullptr)
        {
            return 0;
        }
        const std::string context = module.file + ": " + what + " (group " + std::to_string(group) + " binding " +
            std::to_string(binding) + ", struct " + resource->type + ")";
        const Placement placed = PlaceStruct(module, resource->type);
        for (const std::string& problem : placed.problems)
        {
            ADD_FAILURE() << context << ": " << problem;
        }
        EXPECT_TRUE(placed.layout.known) << context;
        EXPECT_EQ(placed.members.size(), table.size()) << context << ": another number of members than the table's";
        const std::size_t count = std::min(placed.members.size(), table.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            const Gpu::Uniform& expected = table[i];
            const PlacedMember& actual = placed.members[i];
            EXPECT_EQ(actual.name, std::string(expected.name)) << context << ": member " << i;
            EXPECT_EQ(actual.offset, expected.offset) << context << ": offset of " << expected.name;
            EXPECT_EQ(actual.size, expected.size) << context << ": size of " << expected.name;
            EXPECT_EQ(actual.arrayCount, expected.arrayCount) << context << ": array count of " << expected.name;
            if (expected.type != Gpu::UniformType::Struct)
            {
                EXPECT_EQ(actual.type, std::string(WgslTypeOf(expected.type))) << context << ": type of " << expected.name;
            }
            else
            {
                EXPECT_EQ(actual.type.rfind("array<", 0), 0u) << context << ": " << expected.name << " is an array of structs";
            }
        }
        return placed.end;
    }

    bool StructDeclares(const Module& module, const std::string& structName, const std::string& member)
    {
        const std::vector<Field>* fields = module.Struct(structName);
        if (fields == nullptr)
        {
            return false;
        }
        return std::any_of(fields->begin(), fields->end(), [&](const Field& field) { return field.name == member; });
    }

    /// The struct behind the var<uniform> at group 1 binding 0 (every shader's material block)
    std::string MaterialStructOf(const Module& module)
    {
        const Resource* resource = module.At(1, 0);
        return resource == nullptr ? std::string{} : resource->type;
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

    struct ShaderFile
    {
        Gpu::BuiltInShader shader;
        const char* path;
        const char* label;
        std::string_view embedded;
    };

    const ShaderFile AllShaders[] = {
        {Gpu::BuiltInShader::Unlit, "shaders/webgpu/unlit.wgsl", "unlit.wgsl", Renderer::WebGpu::Wgsl::kUnlitWgsl},
        {Gpu::BuiltInShader::Lit, "shaders/webgpu/lit.wgsl", "lit.wgsl", Renderer::WebGpu::Wgsl::kLitWgsl},
        {Gpu::BuiltInShader::Text, "shaders/webgpu/text.wgsl", "text.wgsl", Renderer::WebGpu::Wgsl::kTextWgsl},
    };

    /// The body of fs_main: from its attribute to the end of the module (it is the last function of every file)
    std::string FragmentBody(const Module& module)
    {
        return From(module.text, "@fragment");
    }

    std::size_t CountOf(const std::string& text, const std::string& needle)
    {
        std::size_t count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        {
            ++count;
        }
        return count;
    }
}

TEST(WebGpuShaderParityTest, TheSourcesAreFoundAndPlainAscii)
{
    for (const ShaderFile& file : AllShaders)
    {
        const std::string text = ReadFile(file.path);
        ASSERT_FALSE(text.empty()) << file.path << " (N2_RENDERER_SOURCE_DIR is " << N2_RENDERER_SOURCE_DIR << ")";
        // The generated header cuts the text into raw string literals at arbitrary characters: a multi-byte character
        // could be cut in two, so the files are ASCII
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            ASSERT_LT(static_cast<unsigned char>(text[i]), 128u) << file.path << ": a non-ASCII byte at offset " << i;
        }
        const Module module = ParseWgsl(file.path);
        EXPECT_TRUE(module.vertex.found) << file.label << ": an @vertex function taking one struct and returning one";
        EXPECT_TRUE(module.fragment.found) << file.label << ": an @fragment function taking one struct and returning @location(0) vec4<f32>";
        EXPECT_EQ(module.vertex.name, "vs_main") << file.label;
        EXPECT_EQ(module.fragment.name, "fs_main") << file.label;
    }
}

TEST(WebGpuShaderParityTest, EveryUniformStructDeclaresTheTablesMembersAtTheOffsetsTheTableGives)
{
    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        const std::string label = file.label;

        const std::uint32_t perDrawEnd = ExpectBlockMatchesTable(module, 0, 0, Gpu::kPerDraw, label + " per-draw block");
        EXPECT_EQ(perDrawEnd, Gpu::kPerDrawBlockSize) << label << ": the per-draw block is 3 mat4x4";

        const std::uint32_t materialEnd =
            ExpectBlockMatchesTable(module, 1, 0, Gpu::MaterialUniforms(file.shader), label + " material block");
        EXPECT_EQ(materialEnd, Gpu::MaterialBlockSize(file.shader)) << label;
        const Placement material = PlaceStruct(module, MaterialStructOf(module));
        EXPECT_LE(material.layout.size, Gpu::kMaterialBlockRange)
            << label << ": one dynamic-offset range (kMaterialBlockRange) covers every material struct, rounded up to its alignment";

        if (file.shader == Gpu::BuiltInShader::Lit)
        {
            const std::uint32_t lightingEnd = ExpectBlockMatchesTable(module, 0, 1, Gpu::LightingUniforms(file.shader), "lit lighting block");
            EXPECT_EQ(lightingEnd, Gpu::kLightingBlockSize);
            const Resource* lighting = module.At(0, 1);
            ASSERT_NE(lighting, nullptr);
            EXPECT_EQ(PlaceStruct(module, lighting->type).layout.size, Gpu::kLightingBlockSize)
                << "lit.wgsl: the lighting struct is 752 bytes, a multiple of its alignment";
        }
        else
        {
            EXPECT_EQ(module.At(0, 1), nullptr) << label << " has no lighting block";
        }
    }
}

TEST(WebGpuShaderParityTest, TheResourcesAreExactlyTheTablesTexturesAndSamplersAtTheirBindings)
{
    // The enum order the binding formula depends on, and the sampler count the sampler bindings are offset by
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Emissive) == 0);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Occlusion) == 1);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::Normal) == 2);
    static_assert(static_cast<int>(Renderer::Common::AuxTexture::MetallicRoughness) == 3);
    static_assert(Gpu::kSamplerCount == 5 && Gpu::kFirstSamplerBinding == 1);

    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        const std::span<const Gpu::Sampler> expected = Gpu::Samplers(file.shader);
        for (const Gpu::Sampler& sampler : expected)
        {
            const Resource* texture = module.At(1, sampler.binding);
            ASSERT_NE(texture, nullptr) << file.label << ": no texture at group 1 binding " << sampler.binding << " (" << sampler.name << ")";
            EXPECT_EQ(texture->name, std::string(sampler.name)) << file.label << ": group 1 binding " << sampler.binding;
            EXPECT_EQ(texture->type, "texture_2d<f32>") << file.label << ": " << sampler.name;
            EXPECT_TRUE(texture->space.empty()) << file.label << ": " << sampler.name << " is not var<...>";

            const std::uint32_t samplerBinding = sampler.binding + Gpu::kSamplerCount;
            const Resource* sampled = module.At(1, samplerBinding);
            ASSERT_NE(sampled, nullptr) << file.label << ": no sampler at group 1 binding " << samplerBinding << " (for " << sampler.name << ")";
            EXPECT_EQ(sampled->name, std::string(sampler.name) + "Sampler") << file.label << ": group 1 binding " << samplerBinding;
            EXPECT_EQ(sampled->type, "sampler") << file.label << ": " << sampler.name << "Sampler";
        }
        // Nothing else: the per-draw block, the lighting block of the lit shader, the material block, a texture and a sampler
        // for each table sampler
        const std::size_t expectedResources = 2 + (file.shader == Gpu::BuiltInShader::Lit ? 1 : 0) + 2 * expected.size();
        EXPECT_EQ(module.resources.size(), expectedResources) << file.label << ": another number of resources";
        for (const Resource& resource : module.resources)
        {
            EXPECT_LE(resource.group, 1u) << file.label << ": " << resource.name << " is in group " << resource.group << " (two groups)";
        }
    }

    // The lit shader's five textures are the base texture and the AuxTexture slots in enum order
    const Module lit = ParseWgsl("shaders/webgpu/lit.wgsl");
    const char* const litTextures[] = {"uTexture", "uEmissiveTexture", "uOcclusionTexture", "uNormalTexture", "uMetallicRoughnessTexture"};
    for (std::uint32_t slot = 0; slot < 5; ++slot)
    {
        const Resource* texture = lit.At(1, 1 + slot);
        ASSERT_NE(texture, nullptr) << "lit.wgsl: group 1 binding " << 1 + slot;
        EXPECT_EQ(texture->name, litTextures[slot]) << "lit.wgsl: group 1 binding " << 1 + slot
                                                    << " (slot 0 is the base texture, then AuxTexture in enum order)";
    }
}

TEST(WebGpuShaderParityTest, TheLightStructsAreCheckedMemberByMemberAtTheirOffsetsAndStrides)
{
    struct StructTable
    {
        const char* wgslName;
        const char* arrayName;
        std::span<const Gpu::Member> members;
        std::uint32_t stride;
        std::uint32_t count;
    };
    const StructTable tables[] = {
        {"DirectionalLight", "uDirectionalLights", Gpu::kDirectionalLightMembers, Gpu::kDirectionalLightStride, Gpu::kDirectionalLightCount},
        {"PointLight", "uPointLights", Gpu::kPointLightMembers, Gpu::kPointLightStride, Gpu::kPointLightCount},
        {"SpotLight", "uSpotLights", Gpu::kSpotLightMembers, Gpu::kSpotLightStride, Gpu::kSpotLightCount},
    };

    const Module lit = ParseWgsl("shaders/webgpu/lit.wgsl");
    const Resource* lighting = lit.At(0, 1);
    ASSERT_NE(lighting, nullptr);
    const std::vector<Field>* lightingFields = lit.Struct(lighting->type);
    ASSERT_NE(lightingFields, nullptr);

    for (const StructTable& table : tables)
    {
        const Placement placed = PlaceStruct(lit, table.wgslName);
        for (const std::string& problem : placed.problems)
        {
            ADD_FAILURE() << "lit.wgsl: " << problem;
        }
        ASSERT_EQ(placed.members.size(), table.members.size()) << "lit.wgsl: " << table.wgslName << " has another number of members";
        for (std::size_t i = 0; i < table.members.size(); ++i)
        {
            EXPECT_EQ(placed.members[i].name, std::string(table.members[i].name)) << table.wgslName << " member " << i;
            EXPECT_EQ(placed.members[i].offset, table.members[i].offset) << "lit.wgsl: " << table.wgslName << "." << table.members[i].name;
            EXPECT_EQ(placed.members[i].type, std::string(WgslTypeOf(table.members[i].type))) << "lit.wgsl: " << table.wgslName << "." << table.members[i].name;
        }
        EXPECT_EQ(placed.layout.size, table.stride) << "lit.wgsl: " << table.wgslName << " is " << table.stride << " bytes (the array stride)";

        const auto declared = std::find_if(lightingFields->begin(), lightingFields->end(),
                                           [&](const Field& field) { return field.name == table.arrayName; });
        ASSERT_NE(declared, lightingFields->end()) << "lit.wgsl: the lighting struct has no " << table.arrayName;
        EXPECT_EQ(declared->type, std::string("array<") + table.wgslName + "," + std::to_string(table.count) + ">")
            << "lit.wgsl: " << table.arrayName << " is an array of that struct, of the table's count";
    }
}

TEST(WebGpuShaderParityTest, TheVertexAttributesAreWhereTheTableAndCommonVertexPutThem)
{
    // The table's offsets are Common::Vertex's (also static_asserted in the header)
    EXPECT_EQ(Gpu::kPosition.offset, offsetof(Renderer::Common::Vertex, position));
    EXPECT_EQ(Gpu::kNormal.offset, offsetof(Renderer::Common::Vertex, normal));
    EXPECT_EQ(Gpu::kTexCoord.offset, offsetof(Renderer::Common::Vertex, texCoord));
    EXPECT_EQ(Gpu::kColor.offset, offsetof(Renderer::Common::Vertex, color));
    EXPECT_EQ(Gpu::kTangent.offset, offsetof(Renderer::Common::Vertex, tangent));

    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        const std::vector<Field>* inputs = module.Struct(module.vertex.parameterType);
        ASSERT_NE(inputs, nullptr) << file.label << ": the vertex input struct " << module.vertex.parameterType;
        const std::span<const Gpu::Attribute> attributes = Gpu::Attributes(file.shader);
        EXPECT_EQ(inputs->size(), attributes.size()) << file.label << ": the shader reads exactly the attributes the pipeline gives";
        for (const Gpu::Attribute& attribute : attributes)
        {
            const auto field = std::find_if(inputs->begin(), inputs->end(), [&](const Field& candidate)
                                            { return candidate.location == static_cast<int>(attribute.location); });
            ASSERT_NE(field, inputs->end()) << file.label << ": nothing at @location(" << attribute.location << ") for " << attribute.name;
            EXPECT_EQ(field->name, std::string(attribute.name)) << file.label << ": @location(" << attribute.location << ")";
            EXPECT_EQ(ComponentsOf(field->type), attribute.components) << file.label << ": " << attribute.name << " is " << field->type;
            EXPECT_TRUE(field->builtin.empty()) << file.label << ": " << attribute.name;
        }
    }
}

TEST(WebGpuShaderParityTest, WhatTheVertexStageOutputsIsWhatTheFragmentStageTakes)
{
    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        const std::vector<Field>* outputs = module.Struct(module.vertex.resultType);
        const std::vector<Field>* inputs = module.Struct(module.fragment.parameterType);
        ASSERT_NE(outputs, nullptr) << file.label << ": the vertex output struct " << module.vertex.resultType;
        ASSERT_NE(inputs, nullptr) << file.label << ": the fragment input struct " << module.fragment.parameterType;

        std::size_t positions = 0;
        std::size_t varyings = 0;
        for (const Field& out : *outputs)
        {
            if (out.builtin == "position")
            {
                ++positions;
                EXPECT_EQ(out.type, "vec4<f32>") << file.label << ": the position";
                continue;
            }
            ++varyings;
            ASSERT_GE(out.location, 0) << file.label << ": " << out.name << " has no @location";
            const auto in = std::find_if(inputs->begin(), inputs->end(), [&](const Field& candidate) { return candidate.location == out.location; });
            ASSERT_NE(in, inputs->end()) << file.label << ": the fragment stage takes nothing at @location(" << out.location << ") (" << out.name << ")";
            EXPECT_EQ(in->name, out.name) << file.label << ": @location(" << out.location << ")";
            EXPECT_EQ(in->type, out.type) << file.label << ": @location(" << out.location << ")";
        }
        EXPECT_EQ(positions, 1u) << file.label << ": exactly one @builtin(position) output";
        EXPECT_EQ(inputs->size(), varyings) << file.label << ": the fragment stage reads everything the vertex stage writes";
        EXPECT_EQ(module.fragment.resultType, "vec4<f32>") << file.label << ": the colour output at @location(0)";
    }
}

TEST(WebGpuShaderParityTest, EveryUniformAMaterialSetsIsAMemberOfTheWgslMaterialStructOfTheShaderGpuCacheWouldPick)
{
    // GpuCache picks the lit shader for Lit and Pbr and the unlit shader for Unlit
    const Module lit = ParseWgsl("shaders/webgpu/lit.wgsl");
    const Module unlit = ParseWgsl("shaders/webgpu/unlit.wgsl");

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
            EXPECT_NE(Gpu::FindMaterialUniform(Gpu::BuiltInShader::Lit, name), nullptr) << "the lit table has no entry for " << name;
            EXPECT_TRUE(StructDeclares(lit, MaterialStructOf(lit), name)) << "lit.wgsl: the material struct does not declare " << name;
        }
    }

    const auto unlitMaterial = Material::Create(ShadingModel::Unlit);
    RecordingMaterial recorded;
    unlitMaterial->ApplyUniforms(recorded);
    ASSERT_FALSE(recorded.names.empty());
    for (const std::string& name : recorded.names)
    {
        EXPECT_NE(Gpu::FindMaterialUniform(Gpu::BuiltInShader::Unlit, name), nullptr) << "the unlit table has no entry for " << name;
        EXPECT_TRUE(StructDeclares(unlit, MaterialStructOf(unlit), name)) << "unlit.wgsl: the material struct does not declare " << name;
    }
    EXPECT_FALSE(recorded.names.contains("uPbr")) << "an unlit material sets no lit-shader uniform";
}

TEST(WebGpuShaderParityTest, EveryMaterialDefaultAndTheTextEffectUniformsAreMembersOfTheWgslMaterialStruct)
{
    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        for (const Gpu::MaterialDefault& entry : Gpu::MaterialDefaults(file.shader))
        {
            EXPECT_TRUE(StructDeclares(module, MaterialStructOf(module), std::string(entry.name)))
                << file.label << ": the material struct does not declare the default " << entry.name;
        }
    }
    const Module text = ParseWgsl("shaders/webgpu/text.wgsl");
    for (const char* name : {"uAlbedo", "uOutline", "uOutlineColor", "uSoftness", "uShadowColor", "uShadowOffset", "uShadowSoftness"})
    {
        EXPECT_TRUE(StructDeclares(text, MaterialStructOf(text), name)) << "text.wgsl: " << name;
        EXPECT_NE(Gpu::FindMaterialUniform(Gpu::BuiltInShader::Text, name), nullptr) << name;
    }
}

TEST(WebGpuShaderParityTest, EveryVertexShaderRemapsClipSpaceZFromGlToWebGpuAndMultipliesTheMatricesAsGlDoes)
{
    // The matrices arrive transposed (the backend transposes on upload), so the product reads as in the GLSL
    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        EXPECT_TRUE(Contains(module.text, "clip.z = (clip.z + clip.w) * 0.5;")) << file.label << ": the z remap";
        EXPECT_TRUE(Contains(module.text, "output.position = clip;")) << file.label;
    }
    EXPECT_TRUE(Contains(ParseWgsl("shaders/webgpu/unlit.wgsl").text, "perDraw.uProjection * perDraw.uView * perDraw.uModel * vec4<f32>(input.aPos, 1.0)"));
    EXPECT_TRUE(Contains(ParseWgsl("shaders/webgpu/text.wgsl").text, "perDraw.uProjection * perDraw.uView * perDraw.uModel * vec4<f32>(input.aPos, 1.0)"));
    const Module lit = ParseWgsl("shaders/webgpu/lit.wgsl");
    EXPECT_TRUE(Contains(lit.text, "let worldPos = perDraw.uModel * vec4<f32>(input.aPos, 1.0);"));
    EXPECT_TRUE(Contains(lit.text, "perDraw.uProjection * perDraw.uView * worldPos"));
    // No y flip: WebGPU's clip space is y up with row 0 on top, which is what Vulkan's flipped viewport gives
    for (const ShaderFile& file : AllShaders)
    {
        EXPECT_FALSE(Contains(ParseWgsl(file.path).text, "clip.y = -clip.y")) << file.label;
    }
}

TEST(WebGpuShaderParityTest, TheLitShaderKeepsThePbrConstantsTheEnvironmentBrdfFitAndAnInverseOfTheModelMatrix)
{
    const Module lit = ParseWgsl("shaders/webgpu/lit.wgsl");
    const auto constantOf = [&](const std::string& name)
    {
        std::smatch match;
        const std::regex pattern("const\\s+" + name + "\\s*:\\s*f32\\s*=\\s*([0-9.eE+-]+)\\s*;");
        return std::regex_search(lit.text, match, pattern) ? std::stod(match[1].str()) : -1.0;
    };
    EXPECT_NEAR(constantOf("MIN_PERCEPTUAL_ROUGHNESS"), Renderer::Common::kMinPerceptualRoughness, 1e-6);
    EXPECT_NEAR(constantOf("DIELECTRIC_F0"), Renderer::Common::kDielectricF0, 1e-6);
    EXPECT_NEAR(constantOf("PI"), Renderer::Common::kPi, 1e-6);

    for (const char* coefficient : {"vec4<f32>(-1.0, -0.0275, -0.572, 0.022)", "vec4<f32>(1.0, 0.0425, 1.04, -0.04)",
                                    "exp2(-9.28 * nDotV)", "vec2<f32>(-1.04, 1.04)"})
    {
        EXPECT_TRUE(Contains(lit.text, coefficient)) << coefficient;
    }
    // A Lit material never samples the metallic-roughness texture or uses its channels
    EXPECT_EQ(CountOf(Normalised(lit.text), "if (material.uPbr != 0 && material.uHasMetallicRoughnessTexture != 0)"), 2u);
    for (const char* function : {"fn ggxDistribution(nDotH: f32, alpha: f32) -> f32 {",
                                 "fn smithGgxVisibility(nDotL: f32, nDotV: f32, alpha: f32) -> f32 {",
                                 "fn schlickWeight(vDotH: f32) -> f32 {", "fn linearToSrgb(value: vec3<f32>) -> vec3<f32> {"})
    {
        EXPECT_TRUE(Contains(lit.text, function)) << function;
    }
    // The lighting loops (counts from the lighting block) and the Blinn-Phong shininess
    for (const char* loop : {"for (var i: i32 = 0; i < sceneLighting.uNumDirectionalLights; i++)",
                             "for (var i: i32 = 0; i < sceneLighting.uNumPointLights; i++)",
                             "for (var i: i32 = 0; i < sceneLighting.uNumSpotLights; i++)",
                             "let shininess = mix(4.0, 256.0, material.uSmoothness);"})
    {
        EXPECT_TRUE(Contains(lit.text, loop)) << loop;
    }

    // WGSL has no inverse(): the helper is the adjugate (the cross products of the columns, as rows) over the determinant,
    // and the normal matrix is the transpose of it, as in the GLSL
    for (const char* line : {"fn inverse3(m: mat3x3<f32>) -> mat3x3<f32> {", "let row0 = cross(m[1], m[2]);", "let row1 = cross(m[2], m[0]);",
                             "let row2 = cross(m[0], m[1]);", "let determinantOfM = dot(m[0], row0);",
                             "return transpose(mat3x3<f32>(row0, row1, row2)) * (1.0 / determinantOfM);",
                             "let normalMatrix = transpose(inverse3(model3));",
                             "let model3 = mat3x3<f32>(perDraw.uModel[0].xyz, perDraw.uModel[1].xyz, perDraw.uModel[2].xyz);",
                             "select(1.0, -1.0, determinant(model3) < 0.0)"})
    {
        EXPECT_TRUE(Contains(lit.text, line)) << line;
    }
}

TEST(WebGpuShaderParityTest, TheTextPortKeepsTheAntialiasedSdfMathsAndTheUnlitPortTheAlphaTest)
{
    const Module text = ParseWgsl("shaders/webgpu/text.wgsl");
    for (const char* line : {"let sdf = textureSample(uTexture, uTextureSampler, input.fragTexCoord).r;",
                             "let edgeWidth = max(max(0.5 * fwidth(sdf), 1e-4), material.uSoftness);",
                             "let coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, sdf);",
                             "let color = material.uAlbedo * input.fragColor;", "let alpha = color.a * coverage;",
                             "if (material.uOutline > 0.0) {", "let outlineEdge = 0.5 - material.uOutline;",
                             "if (material.uShadowColor.a > 0.0) {",
                             "textureSample(uTexture, uTextureSampler, input.fragTexCoord - material.uShadowOffset).r;",
                             "if (result.a < 0.01) {", "discard;"})
    {
        EXPECT_TRUE(Contains(text.text, line)) << "text.wgsl: " << line;
    }

    const Module unlit = ParseWgsl("shaders/webgpu/unlit.wgsl");
    for (const char* line : {"var color = material.uAlbedo;", "if (material.uHasTexture != 0) {",
                             "color = color * textureSample(uTexture, uTextureSampler, input.fragTexCoord);",
                             "color = color * input.fragColor;", "if (color.a < material.uAlphaCutoff) {", "discard;"})
    {
        EXPECT_TRUE(Contains(unlit.text, line)) << "unlit.wgsl: " << line;
    }
}

TEST(WebGpuShaderParityTest, EveryDerivativeComesBeforeTheFirstDiscardAndInTheLitShaderBeforeTheLightingLoops)
{
    // WGSL's uniformity analysis rejects a textureSample or fwidth in non-uniform control flow; the shaders keep every one
    // before the first discard and (lit) before the loops that a non-uniform continue leaves non-uniform. Tint is the proof
    // (CI); this keeps a later edit from moving one without the validator being the first to say so.
    for (const ShaderFile& file : AllShaders)
    {
        const Module module = ParseWgsl(file.path);
        const std::string fragment = FragmentBody(module);
        ASSERT_FALSE(fragment.empty()) << file.label;
        const std::size_t discard = fragment.find("discard;");
        ASSERT_NE(discard, std::string::npos) << file.label << ": the alpha test";
        std::size_t samples = 0;
        for (const char* call : {"textureSample(", "fwidth("})
        {
            for (std::size_t at = fragment.find(call); at != std::string::npos; at = fragment.find(call, at + 1))
            {
                ++samples;
                EXPECT_LT(at, discard) << file.label << ": a " << call << " after the first discard, at offset " << at << " of fs_main";
            }
        }
        EXPECT_GE(samples, 1u) << file.label;
    }

    const std::string lit = FragmentBody(ParseWgsl("shaders/webgpu/lit.wgsl"));
    const std::size_t firstLoop = lit.find("for (");
    ASSERT_NE(firstLoop, std::string::npos);
    EXPECT_EQ(CountOf(lit, "textureSample("), 5u) << "the five textures";
    for (std::size_t at = lit.find("textureSample("); at != std::string::npos; at = lit.find("textureSample(", at + 1))
    {
        EXPECT_LT(at, firstLoop) << "lit.wgsl: a textureSample inside or after the lighting loops, at offset " << at << " of fs_main";
    }
}

TEST(WebGpuShaderParityTest, TheWgslEmbeddedByCMakeIsTheFilesText)
{
    for (const ShaderFile& file : AllShaders)
    {
        const std::string text = WithoutCarriageReturns(ReadFile(file.path));
        // The generated header is written with the platform's line ends (CRLF on Windows): the shader text is the same
        // without them (a carriage return is whitespace in WGSL)
        EXPECT_EQ(WithoutCarriageReturns(file.embedded), text) << file.label << ": the generated header is stale or wrong";
        EXPECT_GT(file.embedded.size(), 1000u) << file.label;
    }
}
