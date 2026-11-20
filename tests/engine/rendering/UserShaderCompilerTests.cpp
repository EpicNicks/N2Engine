#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <renderer/common/UserShader.hpp>

#include "engine/ProjectSettings.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/UserShaderCompiler.hpp"

// User shaders, stage 7 (orchestrator/proposals/user-shaders.md section 7): compiling a .n2shader on the device. The C++ twin of the cook
// (UserShaderCompiler) is checked against the cook's own output, committed in tests/renderer/fixtures/user-shaders (tools/n2shader's
// real output): the OpenGL text byte for byte, and, in a build that links glslang (N2ENGINE_RUNTIME_SHADER_COMPILER), the Vulkan
// SPIR-V byte for byte when the linked glslang is the one that cooked the fixtures (the pin in cmake/Glslang.cmake is that version).
namespace
{
    using N2Engine::Rendering::RenderSettings;
    using N2Engine::Rendering::Shader;
    using N2Engine::Rendering::UserShaderCompiler::BuildSource;
    using Renderer::Common::UserShaderTarget;

    const std::vector<std::string> kFixtures = {"solid", "textured", "ambient", "position", "varyings"};

    std::string Dir() { return std::string(N2_USER_SHADER_FIXTURES_DIR) + "/"; }

    std::string ReadText(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(in.good()) << path;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string lf;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '\r' || i + 1 == text.size() || text[i + 1] != '\n')
            {
                lf.push_back(text[i]);
            }
        }
        return lf; // a Windows checkout may have converted the fixtures to CRLF
    }

    std::vector<std::uint8_t> ReadBytes(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(in.good()) << path;
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::shared_ptr<Shader> LoadFixture(const std::string &name)
    {
        auto shader = Shader::FromSource(ReadText(Dir() + name + ".n2shader"), name);
        EXPECT_NE(shader, nullptr) << name;
        if (shader)
        {
            shader->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here"); // nothing cooked: the runtime path
        }
        return shader;
    }

    struct Restore
    {
        bool compile = RenderSettings::GetRuntimeShaderCompile();
        std::filesystem::path cache = RenderSettings::GetShaderCacheDirectory();
        ~Restore()
        {
            RenderSettings::SetRuntimeShaderCompile(compile);
            RenderSettings::SetShaderCacheDirectory(cache);
        }
    };

    std::filesystem::path FreshFolder(const char *name)
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / name;
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        return folder;
    }
}

TEST(UserShaderCompilerTest, Sha256MatchesTheKnownVectors)
{
    using N2Engine::Rendering::UserShaderCompiler::Sha256Hex;
    EXPECT_EQ(Sha256Hex("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Sha256Hex("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string longer = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(Sha256Hex(longer.data(), longer.size()), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(Sha256Hex(million.data(), million.size()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(UserShaderCompilerTest, TheStageIsReadLikeTheCooksParser)
{
    const auto shader = Shader::FromSource("\xEF\xBB\xBF{ \"n2shader\": 1 }\r\n#stage fragment\r\nvoid main() {}\r\n", "t");
    ASSERT_NE(shader, nullptr);
    std::string source;
    int firstLine = 0;
    ASSERT_TRUE(shader->GetStage(source, firstLine));
    EXPECT_EQ(source, "void main() {}\n") << "the stage: every line after the stage line, LF, no byte order mark";
    EXPECT_EQ(firstLine, 3) << "the file line of the stage's first line (the header is line 1, the stage line 2)";
    EXPECT_EQ(shader->GetFileText().find('\r'), std::string::npos);

    const auto none = Shader::FromSource("{ \"n2shader\": 1 }\n#stage fragment  \nvoid main() {}\n", "t");
    if (none) // the engine's header reader is forgiving about trailing spaces; the cook is exact, so there is no stage here
    {
        EXPECT_FALSE(none->GetStage(source, firstLine));
    }
}

TEST(UserShaderCompilerTest, TheOpenGlTextIsByteForByteWhatTheCookWrote)
{
    for (const std::string &name : kFixtures)
    {
        const auto shader = LoadFixture(name);
        ASSERT_NE(shader, nullptr);
        std::string error;
        const auto text = BuildSource(UserShaderTarget::OpenGl, *shader, name + ".n2shader", error);
        ASSERT_TRUE(text.has_value()) << name << ": " << error;
        EXPECT_EQ(*text, ReadText(Dir() + name + "/shader.gl.frag")) << name;
    }
}

TEST(UserShaderCompilerTest, TheVulkanAndWebGpuTextsShareTheUserCodeAndTheBlock)
{
    const auto shader = LoadFixture("solid");
    ASSERT_NE(shader, nullptr);
    std::string error;
    const auto vulkan = BuildSource(UserShaderTarget::Vulkan, *shader, "assets\\fx\\solid.n2shader", error);
    const auto webgpu = BuildSource(UserShaderTarget::WebGpu, *shader, "solid.n2shader", error);
    ASSERT_TRUE(vulkan && webgpu) << error;
    EXPECT_TRUE(vulkan->starts_with("#version 450\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n"));
    EXPECT_NE(vulkan->find("// n2shader contract 1, target vulkan\n"), std::string::npos);
    EXPECT_NE(vulkan->find("layout(set = 1, binding = 0, std140) uniform Material {\n    vec4 u_Color;\n    float u_Gain;\n    vec3 u_Offset;\n    int u_Flag;\n"),
              std::string::npos);
    EXPECT_NE(vulkan->find("#line 12 \"assets/fx/solid.n2shader\"\n"), std::string::npos) << "a backslash in the cited path is a slash";
    EXPECT_NE(webgpu->find("target webgpu"), std::string::npos);
    // the author's lines are the same in both, after the #line
    const auto after = [](const std::string &text) { return text.substr(text.find("#line ")); };
    EXPECT_EQ(after(*vulkan).substr(after(*vulkan).find('\n')), after(*webgpu).substr(after(*webgpu).find('\n')));
}

TEST(UserShaderCompilerTest, TheCacheKeyChangesWithTheSourceAndTheTarget)
{
    using N2Engine::Rendering::UserShaderCompiler::CacheKey;
    const auto solid = LoadFixture("solid");
    const auto textured = LoadFixture("textured");
    ASSERT_TRUE(solid && textured);
    EXPECT_EQ(CacheKey(UserShaderTarget::OpenGl, *solid), CacheKey(UserShaderTarget::OpenGl, *solid));
    EXPECT_NE(CacheKey(UserShaderTarget::OpenGl, *solid), CacheKey(UserShaderTarget::OpenGl, *textured));
    EXPECT_NE(CacheKey(UserShaderTarget::OpenGl, *solid), CacheKey(UserShaderTarget::Vulkan, *solid));
    EXPECT_EQ(CacheKey(UserShaderTarget::OpenGl, *solid).size(), 64u);
}

TEST(UserShaderCompilerTest, OpenGlIsBuiltFromTheSourceWhenNothingIsCookedAndKeptInTheCache)
{
    const Restore restore;
    const std::filesystem::path cache = FreshFolder("n2-shader-cache-gl");
    RenderSettings::SetShaderCacheDirectory(cache);
    RenderSettings::SetRuntimeShaderCompile(false); // not needed for OpenGL: its payload is text

    const auto shader = LoadFixture("textured");
    ASSERT_NE(shader, nullptr);
    std::string error;
    const auto blob = shader->MakeBlob(UserShaderTarget::OpenGl, error);
    ASSERT_TRUE(blob.has_value()) << error;
    EXPECT_EQ(blob->fragmentGlsl, ReadText(Dir() + "textured/shader.gl.frag"));
    EXPECT_EQ(blob->params.size(), 4u);
    EXPECT_EQ(blob->blockSize, 16u);

    // one entry in the cache: the manifest and the payload
    std::size_t entries = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cache))
    {
        ++entries;
        EXPECT_TRUE(std::filesystem::exists(entry.path() / "manifest.json"));
        EXPECT_TRUE(std::filesystem::exists(entry.path() / "shader.gl.frag"));
    }
    EXPECT_EQ(entries, 1u);

    // the second blob comes from the cache; a payload that is not the hashed one is ignored and made again
    const auto again = shader->MakeBlob(UserShaderTarget::OpenGl, error);
    ASSERT_TRUE(again.has_value()) << error;
    EXPECT_EQ(again->fragmentGlsl, blob->fragmentGlsl);
    for (const auto &entry : std::filesystem::directory_iterator(cache))
    {
        std::ofstream(entry.path() / "shader.gl.frag", std::ios::binary | std::ios::trunc) << "#version 330 core\nvoid main() {}\n";
    }
    const auto repaired = shader->MakeBlob(UserShaderTarget::OpenGl, error);
    ASSERT_TRUE(repaired.has_value()) << error;
    EXPECT_EQ(repaired->fragmentGlsl, blob->fragmentGlsl) << "a tampered cache entry is never used";
    std::filesystem::remove_all(cache);
}

TEST(UserShaderCompilerTest, WithoutASourceOrForWebGpuThereIsNothingToBuildFrom)
{
    const Restore restore;
    RenderSettings::SetShaderCacheDirectory({});
    const auto stripped = Shader::FromSource("{ \"n2shader\": 1, \"name\": \"S\" }\n#stage fragment\n", "stripped");
    ASSERT_NE(stripped, nullptr);
    stripped->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here");
    std::string error;
    EXPECT_FALSE(stripped->MakeBlob(UserShaderTarget::OpenGl, error).has_value());
    EXPECT_NE(error.find("stripped"), std::string::npos) << error;

    const auto solid = LoadFixture("solid");
    ASSERT_NE(solid, nullptr);
    EXPECT_FALSE(solid->MakeBlob(UserShaderTarget::WebGpu, error).has_value());
    EXPECT_NE(error.find("WGSL"), std::string::npos) << error;
}

TEST(UserShaderCompilerTest, VulkanNeedsTheLinkedCompilerAndTheSetting)
{
    const Restore restore;
    RenderSettings::SetShaderCacheDirectory({});
    const auto shader = LoadFixture("solid");
    ASSERT_NE(shader, nullptr);
    std::string error;
    RenderSettings::SetRuntimeShaderCompile(false);
    EXPECT_FALSE(shader->MakeBlob(UserShaderTarget::Vulkan, error).has_value());
    if (N2Engine::Rendering::UserShaderCompiler::IsAvailable())
    {
        EXPECT_NE(error.find("rendering.runtimeShaderCompile"), std::string::npos) << error;
    }
    else
    {
        EXPECT_NE(error.find("no glslang"), std::string::npos) << error;
        RenderSettings::SetRuntimeShaderCompile(true);
        EXPECT_FALSE(shader->MakeBlob(UserShaderTarget::Vulkan, error).has_value());
        EXPECT_NE(error.find("no glslang"), std::string::npos) << error;
    }
}

TEST(UserShaderCompilerTest, TheProjectSettingTurnsRuntimeCompileOnAndOff)
{
    const Restore restore;
    RenderSettings::SetRuntimeShaderCompile(false);
    // an absent value leaves what the host chose
    (void)N2Engine::ApplyProjectSettings(nlohmann::json{{"rendering", nlohmann::json::object()}});
    EXPECT_FALSE(RenderSettings::GetRuntimeShaderCompile());
    RenderSettings::SetRuntimeShaderCompile(true);
    (void)N2Engine::ApplyProjectSettings(nlohmann::json{{"rendering", nlohmann::json::object()}});
    EXPECT_TRUE(RenderSettings::GetRuntimeShaderCompile());
    (void)N2Engine::ApplyProjectSettings(nlohmann::json{{"rendering", {{"runtimeShaderCompile", false}}}});
    EXPECT_FALSE(RenderSettings::GetRuntimeShaderCompile());
    (void)N2Engine::ApplyProjectSettings(nlohmann::json{{"rendering", {{"runtimeShaderCompile", true}}}});
    EXPECT_TRUE(RenderSettings::GetRuntimeShaderCompile());
    const auto problems = N2Engine::ApplyProjectSettings(nlohmann::json{{"rendering", {{"runtimeShaderCompile", "yes"}}}});
    EXPECT_FALSE(problems.empty());
    EXPECT_TRUE(RenderSettings::GetRuntimeShaderCompile()) << "a wrong type changes nothing";
}

// ---- in a build with glslang (N2ENGINE_RUNTIME_SHADER_COMPILER): the cross-check against the node cook's committed output ----

TEST(UserShaderCompilerTest, VulkanSpirvIsTheCooksWhenTheCompilerIsTheSame)
{
    using N2Engine::Rendering::UserShaderCompiler::CompilerId;
    using N2Engine::Rendering::UserShaderCompiler::IsAvailable;
    if (!IsAvailable())
    {
        GTEST_SKIP() << "this build has no glslang (N2ENGINE_RUNTIME_SHADER_COMPILER)";
    }
    const Restore restore;
    RenderSettings::SetShaderCacheDirectory({});
    RenderSettings::SetRuntimeShaderCompile(true);
    for (const std::string &name : kFixtures)
    {
        const auto shader = LoadFixture(name);
        ASSERT_NE(shader, nullptr);
        std::string error;
        const auto blob = shader->MakeBlob(UserShaderTarget::Vulkan, error);
        ASSERT_TRUE(blob.has_value()) << name << ": " << error;
        EXPECT_TRUE(Renderer::Common::ValidateUserShaderBlob(*blob, UserShaderTarget::Vulkan).empty()) << name;

        const nlohmann::json manifest = nlohmann::json::parse(ReadText(Dir() + name + "/manifest.json"));
        EXPECT_EQ(blob->blockSize, manifest.at("blockSize").get<std::uint32_t>()) << name;
        ASSERT_EQ(blob->params.size(), manifest.at("params").size()) << name;
        for (std::size_t i = 0; i < blob->params.size(); ++i)
        {
            const nlohmann::json &cooked = manifest.at("params").at(i);
            EXPECT_EQ(blob->params[i].name, cooked.at("name").get<std::string>()) << name;
            if (cooked.contains("offset"))
            {
                EXPECT_EQ(blob->params[i].offset, cooked.at("offset").get<std::uint32_t>()) << name << "." << blob->params[i].name;
                EXPECT_EQ(blob->params[i].size, cooked.at("size").get<std::uint32_t>()) << name << "." << blob->params[i].name;
            }
        }

        const std::string cookedWith = manifest.at("tools").at("glslang").get<std::string>();
        const std::vector<std::uint8_t> cooked = ReadBytes(Dir() + name + "/shader.vulkan.frag.spv");
        std::vector<std::uint8_t> ours;
        for (const std::uint32_t word : blob->fragmentSpirv)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                ours.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        if (cookedWith == CompilerId())
        {
            EXPECT_EQ(ours, cooked) << name << ": the same glslang (" << cookedWith << ") and flags must give the cook's bytes";
        }
        else
        {
            std::cout << "[  NOTE  ] " << name << ": the fixtures were cooked with '" << cookedWith << "', this build has '" << CompilerId()
                      << "'; the SPIR-V was checked for validity and layout, not bytes\n";
            EXPECT_EQ(ours.size() % 4, 0u);
        }
    }
}

TEST(UserShaderCompilerTest, ACompileErrorCitesTheFileAndLineAndMakesNoBlob)
{
    if (!N2Engine::Rendering::UserShaderCompiler::IsAvailable())
    {
        GTEST_SKIP() << "this build has no glslang (N2ENGINE_RUNTIME_SHADER_COMPILER)";
    }
    const Restore restore;
    RenderSettings::SetShaderCacheDirectory({});
    RenderSettings::SetRuntimeShaderCompile(true);
    const auto shader = Shader::FromSource("{ \"n2shader\": 1, \"name\": \"Bad\" }\n#stage fragment\nvoid main() {\n    n2_FragColor = vec4(oops);\n}\n", "bad");
    ASSERT_NE(shader, nullptr);
    shader->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here");
    std::string error;
    EXPECT_FALSE(shader->MakeBlob(UserShaderTarget::Vulkan, error).has_value());
    EXPECT_NE(error.find("compiling it here failed"), std::string::npos) << error;
    EXPECT_NE(error.find(":4:"), std::string::npos) << "the author's line 4 (#line gives glslang the file's numbering): " << error;
    EXPECT_NE(error.find("oops"), std::string::npos) << error;
}

TEST(UserShaderCompilerTest, VulkanSpirvIsKeptInTheCacheAndReadBack)
{
    if (!N2Engine::Rendering::UserShaderCompiler::IsAvailable())
    {
        GTEST_SKIP() << "this build has no glslang (N2ENGINE_RUNTIME_SHADER_COMPILER)";
    }
    const Restore restore;
    const std::filesystem::path cache = FreshFolder("n2-shader-cache-vk");
    RenderSettings::SetShaderCacheDirectory(cache);
    RenderSettings::SetRuntimeShaderCompile(true);
    const auto shader = LoadFixture("ambient");
    ASSERT_NE(shader, nullptr);
    std::string error;
    const auto first = shader->MakeBlob(UserShaderTarget::Vulkan, error);
    ASSERT_TRUE(first.has_value()) << error;
    EXPECT_TRUE(first->usesLighting);
    std::size_t entries = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cache))
    {
        ++entries;
        EXPECT_TRUE(std::filesystem::exists(entry.path() / "shader.vulkan.frag.spv"));
    }
    EXPECT_EQ(entries, 1u);
    // a changed file is another key: a second entry, the first one is left (the cache is pruned by count)
    const auto other = LoadFixture("solid");
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(other->MakeBlob(UserShaderTarget::Vulkan, error).has_value()) << error;
    entries = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(cache))
    {
        ++entries;
    }
    EXPECT_EQ(entries, 2u);
    const auto second = shader->MakeBlob(UserShaderTarget::Vulkan, error);
    ASSERT_TRUE(second.has_value()) << error;
    EXPECT_EQ(second->fragmentSpirv, first->fragmentSpirv);
    std::filesystem::remove_all(cache);
}
