#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/UserShader.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWShader.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/rendering/UserShaderSlot.hpp"

#include "MeshTestSupport.hpp"

// User shaders, stage 5a (orchestrator/proposals/user-shaders.md): the Shader asset (a .n2shader's header, and the cooked payload turned
// into the blob a backend takes), the Material's shader and typed parameters and their .mat keys, and what GpuCache does with a
// material that has a shader: a user-shader material on a backend that has them, the unlit fallback (magenta in the editor) where it
// cannot be drawn, and nothing changed for a material without one. The GPU backends' .mat-to-pixels goldens are in
// tests/renderer (Vulkan, WebGPU); the software one is at the end of this file. The cooked fixtures are
// tests/renderer/fixtures/user-shaders (tools/n2shader's real output).

using namespace N2Engine;
using Rendering::GpuCache;
using Rendering::Material;
using Rendering::MaterialParamValue;
using Rendering::Shader;
using Rendering::ShaderParamType;
using Rendering::ShadingModel;
using Rendering::Texture;
using Rendering::UserShaderErrorStyle;
using MeshTestSupport::RecordingMeshRenderer;
using MeshTestSupport::WarningCapture;
using Renderer::Common::UserShaderBlob;
using Renderer::Common::UserShaderTarget;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    fs::path FixtureDir() { return fs::path(N2_USER_SHADER_FIXTURES_DIR); }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        EXPECT_TRUE(static_cast<bool>(file)) << path;
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::string FixtureSource(const std::string &name) { return ReadFile(FixtureDir() / (name + ".n2shader")); }

    /// A shader from a fixture's source, reading its cooked payload from the fixture's folder
    std::shared_ptr<Shader> FixtureShader(const std::string &name)
    {
        auto shader = Shader::FromSource(FixtureSource(name), name + ".n2shader");
        EXPECT_NE(shader, nullptr) << name;
        if (shader)
        {
            shader->SetCookedDirectory(FixtureDir() / name);
        }
        return shader;
    }

    std::shared_ptr<Material> ShaderMaterial(const std::string &fixture)
    {
        auto material = Material::Create(ShadingModel::Lit);
        material->SetShader(FixtureShader(fixture));
        return material;
    }

    /// A material of a user shader as a backend makes it: it takes the declared parameters and says it is a user material
    class FakeUserMaterial final : public Renderer::Software::SWMaterial
    {
    public:
        using SWMaterial::SWMaterial;
        [[nodiscard]] bool IsUserShaderMaterial() const override { return true; }

        void SetInt(const std::string &n, int v) override { names.insert(n); SWMaterial::SetInt(n, v); ints[n] = v; }
        void SetFloat(const std::string &n, float v) override { names.insert(n); SWMaterial::SetFloat(n, v); floats[n] = v; }
        void SetVec2(const std::string &n, float x, float y) override { names.insert(n); vecs[n] = {x, y, 0, 0}; }
        void SetVec3(const std::string &n, float x, float y, float z) override { names.insert(n); vecs[n] = {x, y, z, 0}; }
        void SetVec4(const std::string &n, float x, float y, float z, float w) override { names.insert(n); vecs[n] = {x, y, z, w}; }
        void SetColor(const std::string &n, float r, float g, float b, float a) override { names.insert(n); vecs[n] = {r, g, b, a}; }
        void SetAuxTexture(Renderer::Common::AuxTexture which, Renderer::Common::ITexture *t) override
        {
            slots[1 + static_cast<std::size_t>(which)] = t;
        }
        void SetTexture(Renderer::Common::ITexture *t) override { slots[0] = t; SWMaterial::SetTexture(t); }

        std::set<std::string> names;
        std::map<std::string, int> ints;
        std::map<std::string, float> floats;
        std::map<std::string, std::array<float, 4>> vecs;
        std::array<Renderer::Common::ITexture *, 5> slots{};
    };

    /// A renderer with user shaders: it records the blobs it is given and the order things are made and destroyed in
    class UserShaderRenderer : public RecordingMeshRenderer
    {
    public:
        std::optional<UserShaderTarget> target = UserShaderTarget::Vulkan;
        bool refuseShaders = false;
        bool refuseMaterials = false;
        std::vector<UserShaderBlob> blobs;
        std::vector<std::unique_ptr<Renderer::Software::SWShader>> userShaders;
        std::vector<std::unique_ptr<FakeUserMaterial>> userMaterials;
        std::vector<std::string> events;

        [[nodiscard]] std::optional<UserShaderTarget> GetUserShaderTarget() const override { return target; }

        Renderer::Common::IShader *CreateUserShader(const UserShaderBlob &blob) override
        {
            blobs.push_back(blob);
            events.push_back("createShader");
            if (refuseShaders)
            {
                return nullptr;
            }
            userShaders.push_back(std::make_unique<Renderer::Software::SWShader>(Renderer::Software::SWShaderType::Unlit));
            return userShaders.back().get();
        }

        Renderer::Common::IMaterial *CreateUserMaterial(Renderer::Common::IShader *shader, Renderer::Common::ITexture *texture) override
        {
            events.push_back("createMaterial");
            if (refuseMaterials)
            {
                return nullptr;
            }
            userMaterials.push_back(std::make_unique<FakeUserMaterial>(shader, texture));
            userMaterials.back()->slots[0] = texture;
            return userMaterials.back().get();
        }

        void DestroyMaterial(Renderer::Common::IMaterial *material) override
        {
            if (const auto it = std::ranges::find_if(userMaterials, [material](const auto &p) { return p.get() == material; });
                it != userMaterials.end())
            {
                events.push_back("destroyMaterial");
                userMaterials.erase(it);
                return;
            }
            RecordingMeshRenderer::DestroyMaterial(material);
        }

        bool DestroyShaderProgram(Renderer::Common::IShader *shader) override
        {
            if (const auto it = std::ranges::find_if(userShaders, [shader](const auto &p) { return p.get() == shader; });
                it != userShaders.end())
            {
                events.push_back("destroyShader");
                userShaders.erase(it);
                return true;
            }
            return RecordingMeshRenderer::DestroyShaderProgram(shader);
        }

        [[nodiscard]] const char *GetRendererName() const override { return "UserShaderRenderer"; }
    };

    std::shared_ptr<Texture> OnePixel(const std::uint8_t r)
    {
        return Texture::Create(1, 1, std::vector<std::uint8_t>{r, 20, 30, 255});
    }
}

// ============================================================================
// The Shader asset
// ============================================================================

TEST(ShaderAssetTest, TheFixtureHeadersAreReadWithTheirParametersAndSlots)
{
    const auto solid = FixtureShader("solid");
    ASSERT_NE(solid, nullptr);
    EXPECT_EQ(solid->GetName(), "FixtureSolid");
    EXPECT_EQ(solid->GetResourceType(), "Shader");
    ASSERT_EQ(solid->GetParams().size(), 6u);
    EXPECT_EQ(solid->GetParams()[0].name, "u_Color");
    EXPECT_EQ(solid->GetParams()[0].type, ShaderParamType::Color);
    EXPECT_EQ(solid->GetParams()[0].defaultValue, (std::array<float, 4>{1, 0, 0, 1}));
    EXPECT_EQ(solid->GetParams()[3].type, ShaderParamType::Bool);
    EXPECT_EQ(solid->GetParams()[4].defaultValue[0], 2.0f);
    EXPECT_FALSE(solid->UsesLighting());

    const auto textured = FixtureShader("textured");
    ASSERT_NE(textured, nullptr);
    EXPECT_EQ(textured->FindParam("u_Tex")->slot, 0);
    EXPECT_EQ(textured->FindParam("u_Add")->slot, 1);
    EXPECT_EQ(textured->FindParam("u_Normal")->slot, 2);
    EXPECT_EQ(textured->FindParam("u_Add")->defaultTexture, "black");
    EXPECT_EQ(textured->FindParam("u_Normal")->defaultTexture, "normal");
    EXPECT_EQ(textured->FindParam("nope"), nullptr);

    EXPECT_TRUE(FixtureShader("ambient")->UsesLighting());
    EXPECT_TRUE(FixtureShader("position")->GetParams().empty());
}

TEST(ShaderAssetTest, TheReservedBaseColorTextureTakesSlotZeroWhateverItsPlace)
{
    const auto shader = Shader::FromSource(
        R"({ "n2shader": 1, "params": [ {"name": "u_Mask", "type": "texture"}, {"name": "baseColorTexture", "type": "texture"} ] }
#stage fragment
void main() { n2_FragColor = n2_texture(u_Mask, n2_UV) * n2_texture(baseColorTexture, n2_UV); }
)");
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->FindParam("baseColorTexture")->slot, 0);
    EXPECT_EQ(shader->FindParam("u_Mask")->slot, 1);
}

TEST(ShaderAssetTest, AHeaderThatBreaksTheFormatIsRefusedWithAMessageThatNamesTheFile)
{
    const auto refused = [](const std::string &text, const std::string &expectedInMessage)
    {
        WarningCapture capture;
        EXPECT_EQ(Shader::FromSource(text, "broken.n2shader"), nullptr) << text;
        EXPECT_TRUE(capture.Mentions("broken.n2shader")) << text;
        EXPECT_TRUE(capture.Mentions(expectedInMessage)) << text << " -> " << (capture.messages.empty() ? "" : capture.messages[0]);
    };
    refused("{ \"n2shader\": 1 }\nvoid main() {}\n", "#stage fragment");
    refused("{ not json\n#stage fragment\nvoid main() {}\n", "not valid JSON");
    refused("{ \"n2shader\": 2 }\n#stage fragment\nvoid main() {}\n", "format 2");
    refused("{ \"name\": \"x\" }\n#stage fragment\nvoid main() {}\n", "n2shader");
    refused(R"({ "n2shader": 1, "params": [ {"name": "a", "type": "float"}, {"name": "a", "type": "int"} ] }
#stage fragment
void main() {}
)", "declared twice");
    refused(R"({ "n2shader": 1, "params": [ {"name": "n2_x", "type": "float"} ] }
#stage fragment
void main() {}
)", "n2_");
    refused(R"({ "n2shader": 1, "params": [ {"name": "a", "type": "mat4"} ] }
#stage fragment
void main() {}
)", "unknown type");
    refused(R"({ "n2shader": 1, "params": [ {"name": "a", "type": "vec3", "default": [1, 2]} ] }
#stage fragment
void main() {}
)", "3 numbers");
    refused(R"({ "n2shader": 1, "uses": ["time"] }
#stage fragment
void main() {}
)", "lighting");
    std::string many = R"({ "n2shader": 1, "params": [)";
    for (int i = 0; i < 6; ++i)
    {
        many += std::string(i ? "," : "") + R"({"name": "t)" + std::to_string(i) + R"(", "type": "texture"})";
    }
    refused(many + "] }\n#stage fragment\nvoid main() {}\n", "texture parameters");
}

TEST(ShaderAssetTest, TheCookedPayloadOfEveryBackendBecomesTheBlobItTakes)
{
    for (const char *name : {"solid", "textured", "ambient", "position", "varyings"})
    {
        const auto shader = FixtureShader(name);
        ASSERT_NE(shader, nullptr) << name;
        for (const UserShaderTarget target : {UserShaderTarget::Vulkan, UserShaderTarget::WebGpu, UserShaderTarget::OpenGl})
        {
            std::string error;
            const auto blob = shader->MakeBlob(target, error);
            ASSERT_TRUE(blob.has_value()) << name << ": " << error;
            EXPECT_EQ(Renderer::Common::ValidateUserShaderBlob(*blob, target), "") << name;
            EXPECT_EQ(blob->usesLighting, shader->UsesLighting()) << name;
            EXPECT_EQ(blob->params.size(), shader->GetParams().size()) << name;
            // Only the target's payload is filled in
            EXPECT_EQ(!blob->fragmentSpirv.empty(), target == UserShaderTarget::Vulkan) << name;
            EXPECT_EQ(!blob->fragmentWgsl.empty(), target == UserShaderTarget::WebGpu) << name;
            EXPECT_EQ(!blob->fragmentGlsl.empty(), target == UserShaderTarget::OpenGl) << name;
        }
    }
}

TEST(ShaderAssetTest, ACookedPayloadThatDoesNotMatchTheHeaderOrIsMissingIsReportedNotUsed)
{
    std::string error;
    // The header gained a parameter since it was cooked: stale
    auto edited = Shader::FromSource(R"({ "n2shader": 1, "name": "FixtureSolid", "params": [ {"name": "u_Color", "type": "color"}, {"name": "extra", "type": "float"} ] }
#stage fragment
void main() { n2_FragColor = u_Color; }
)");
    ASSERT_NE(edited, nullptr);
    edited->SetCookedDirectory(FixtureDir() / "solid");
    EXPECT_FALSE(edited->MakeBlob(UserShaderTarget::Vulkan, error).has_value());
    EXPECT_NE(error.find("stale"), std::string::npos) << error;

    // No such folder: not cooked
    const auto uncooked = FixtureShader("solid");
    uncooked->SetCookedDirectory(FixtureDir() / "does-not-exist");
    EXPECT_FALSE(uncooked->MakeBlob(UserShaderTarget::Vulkan, error).has_value());
    EXPECT_NE(error.find("no cooked data"), std::string::npos) << error;

    // A manifest that lists no payload for the backend
    const fs::path temp = fs::temp_directory_path() / "n2engine_shader_nopayload";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);
    json manifest = json::parse(ReadFile(FixtureDir() / "solid" / "manifest.json"));
    manifest["payloads"].erase("webgpu");
    std::ofstream(temp / "manifest.json") << manifest.dump(2);
    const auto partial = FixtureShader("solid");
    partial->SetCookedDirectory(temp);
    EXPECT_FALSE(partial->MakeBlob(UserShaderTarget::WebGpu, error).has_value());
    EXPECT_NE(error.find("no payload for the webgpu backend"), std::string::npos) << error;
    EXPECT_NE(error.find("vulkan"), std::string::npos) << "it says what the data was cooked for: " << error;
    fs::remove_all(temp, ec);
}

// ============================================================================
// The Material's shader and parameters
// ============================================================================

TEST(MaterialShaderTest, ParametersAreSetByDeclaredNameAndTypeAndDefaultToTheShadersValues)
{
    auto material = ShaderMaterial("solid");
    EXPECT_EQ(material->GetShader()->GetName(), "FixtureSolid");

    // Defaults come from the shader
    EXPECT_EQ(material->GetParam("u_Gain")->value[0], 1.0f);
    EXPECT_EQ(material->GetParam("u_Color")->value, (std::array<float, 4>{1, 0, 0, 1}));
    EXPECT_FALSE(material->GetParam("nope").has_value());
    EXPECT_TRUE(material->GetParams().empty()) << "nothing was set";

    WarningCapture capture;
    EXPECT_TRUE(material->SetFloat("u_Gain", 2.5f));
    EXPECT_TRUE(material->SetColor("u_Color", Common::Color(0.0f, 1.0f, 0.0f, 0.5f)));
    EXPECT_TRUE(material->SetVec3("u_Offset", 0.1f, 0.2f, 0.3f));
    EXPECT_TRUE(material->SetInt("u_Flag", 7)) << "a bool takes an int: nonzero is true";
    EXPECT_TRUE(material->SetInt("u_Steps", 5));
    EXPECT_TRUE(material->SetVec2("u_Pair", 1.0f, 2.0f));
    EXPECT_TRUE(capture.messages.empty());
    EXPECT_EQ(material->GetParam("u_Gain")->value[0], 2.5f);
    EXPECT_EQ(material->GetParam("u_Flag")->value[0], 1.0f);
    EXPECT_EQ(material->GetParam("u_Color")->value, (std::array<float, 4>{0, 1, 0, 0.5f}));

    // A wrong name or the wrong setter is refused, and warns once per name and setter
    EXPECT_FALSE(material->SetFloat("nope", 1.0f));
    EXPECT_FALSE(material->SetFloat("nope", 2.0f));
    EXPECT_FALSE(material->SetVec3("u_Gain", 1, 2, 3));
    EXPECT_FALSE(material->SetTexture("u_Color", nullptr));
    EXPECT_EQ(capture.messages.size(), 3u);
    EXPECT_TRUE(capture.Mentions("declares no such parameter"));
    EXPECT_TRUE(capture.Mentions("does not fit this setter"));

    // No shader: nothing to set
    auto plain = Material::Create();
    WarningCapture plainCapture;
    EXPECT_FALSE(plain->SetFloat("u_Gain", 1.0f));
    EXPECT_TRUE(plainCapture.Mentions("no shader"));
}

TEST(MaterialShaderTest, MinAndMaxClampAndANonFiniteNumberTakesTheDefault)
{
    auto shader = Shader::FromSource(R"({ "n2shader": 1, "params": [ {"name": "k", "type": "float", "default": 0.25, "min": 0, "max": 1} ] }
#stage fragment
void main() { n2_FragColor = vec4(k); }
)");
    ASSERT_NE(shader, nullptr);
    auto material = Material::Create();
    material->SetShader(shader);
    EXPECT_TRUE(material->SetFloat("k", 5.0f));
    EXPECT_EQ(material->GetParam("k")->value[0], 1.0f);
    EXPECT_TRUE(material->SetFloat("k", -3.0f));
    EXPECT_EQ(material->GetParam("k")->value[0], 0.0f);
    EXPECT_TRUE(material->SetFloat("k", std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(material->GetParam("k")->value[0], 0.25f);
}

TEST(MaterialShaderTest, OnlyTheShaderAndTexturesMakeANewGpuMaterial)
{
    auto material = ShaderMaterial("textured");
    const std::uint64_t gpu = material->GetGpuVersion();
    const std::uint64_t version = material->GetVersion();
    EXPECT_TRUE(material->SetColor("u_Tint", Common::Color(1, 0, 0, 1)));
    EXPECT_EQ(material->GetGpuVersion(), gpu) << "a value is a uniform";
    EXPECT_GT(material->GetVersion(), version);
    EXPECT_TRUE(material->SetTexture("u_Tex", OnePixel(10)));
    EXPECT_EQ(material->GetGpuVersion(), gpu + 1) << "a texture is part of the GPU material";
    material->SetShader(FixtureShader("solid"));
    EXPECT_EQ(material->GetGpuVersion(), gpu + 2);
    EXPECT_TRUE(material->GetParams().empty()) << "the old shader's values mean nothing to the new one";
}

TEST(MaterialShaderTest, ATextureParameterIsStoredAndTheMaterialsBaseColorTextureIsKeptApart)
{
    auto material = ShaderMaterial("textured");
    WarningCapture capture;
    const auto texture = OnePixel(10);
    EXPECT_TRUE(material->SetTexture("u_Tex", texture));
    EXPECT_EQ(material->GetParam("u_Tex")->texture, texture);
    EXPECT_EQ(material->GetBaseColorTexture(), nullptr);
    EXPECT_TRUE(material->SetTexture("u_Tex", nullptr)) << "null clears it";
    EXPECT_EQ(material->GetParam("u_Tex")->texture, nullptr);
    EXPECT_TRUE(capture.messages.empty());
}

TEST(MaterialShaderTest, AMaterialWithoutAShaderWritesExactlyTheKeysItAlwaysDid)
{
    const std::set<std::string> legacy = {"shading",           "baseColor",         "baseColorTexture",         "alphaMode",
                                          "alphaCutoff",       "doubleSided",       "smoothness",               "metallic",
                                          "emissive",          "normalTexture",     "normalScale",              "occlusionTexture",
                                          "occlusionStrength", "metallicRoughnessTexture", "emissiveTexture"};
    const auto keys = [](const json &object)
    {
        std::set<std::string> out;
        for (const auto &[key, value] : object.items())
        {
            out.insert(key);
        }
        return out;
    };
    auto material = Material::Create(ShadingModel::Pbr);
    material->SetBaseColor(Common::Color(0.1f, 0.2f, 0.3f, 1.0f));
    const json saved = material->ToJson();
    EXPECT_EQ(keys(saved), legacy);
    EXPECT_FALSE(saved.contains("shader"));
    EXPECT_FALSE(saved.contains("params"));
    // An old file (without the new keys) reads and writes back the same
    WarningCapture capture;
    const auto loaded = Material::FromJson(saved);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(capture.messages.empty());
    EXPECT_EQ(loaded->ToJson().dump(), saved.dump()) << "byte for byte";
    EXPECT_EQ(loaded->GetShader(), nullptr);

    // With a shader the two keys appear, and only then
    auto withShader = ShaderMaterial("solid");
    const json shaderJson = withShader->ToJson();
    EXPECT_TRUE(shaderJson.contains("shader"));
    ASSERT_TRUE(shaderJson.contains("params"));
    EXPECT_TRUE(shaderJson.at("params").empty());
}

// ============================================================================
// .mat files with a shader, in a project
// ============================================================================

class ShaderMatAssetTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static void WriteText(const std::string &relative, const std::string &text)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_shadermat_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets");
        WriteText("shaders/solid.n2shader", FixtureSource("solid"));
        WriteText("shaders/textured.n2shader", FixtureSource("textured"));
        WriteText("shaders/reserved.n2shader", R"({ "n2shader": 1, "name": "Reserved", "params": [
      { "name": "baseColor", "type": "color", "default": [1, 1, 1, 1] },
      { "name": "u_Mask", "type": "texture", "default": "black" },
      { "name": "baseColorTexture", "type": "texture" } ] }
    #stage fragment
    void main() { n2_FragColor = baseColor * n2_texture(baseColorTexture, n2_UV) * n2_texture(u_Mask, n2_UV); }
    )");
        WriteText("materials/green.mat", R"({
            "shading": "unlit",
            "shader": "../shaders/solid.n2shader",
            "params": { "u_Color": [0, 1, 0, 1], "u_Gain": 2, "u_Offset": [0.1, 0.2, 0.3], "u_Flag": true, "u_Steps": 4.9, "u_Pair": [3, 4] }
        })");
        WriteText("materials/byResPath.mat", R"({"shader": "res://shaders/solid.n2shader"})");
        WriteText("materials/bad.mat", R"({
            "shader": "../shaders/solid.n2shader",
            "params": { "nope": 1, "u_Gain": "fast", "u_Offset": [1, 2], "u_Flag": 1, "u_Color": "red", "u_Pair": [1, 2] }
        })");
        WriteText("materials/orphan.mat", R"({"params": {"u_Gain": 1}})");
        WriteText("materials/missing.mat", R"({"shader": "../shaders/nope.n2shader", "doubleSided": true})");
        WriteText("materials/textured.mat", R"({
            "shader": "../shaders/textured.n2shader",
            "params": { "u_Tex": null }
        })");
        // Stage 9a: one shader per blend hint with a .mat for each, and one .mat that overrides the hint
        for (const std::string hint : {"premultiplied", "multiply", "additive", "alpha"})
        {
            WriteText("shaders/blend_" + hint + ".n2shader",
                      "{ \"n2shader\": 1, \"name\": \"B\", \"state\": { \"blend\": \"" + hint + "\" } }\n" +
                          "#stage fragment\nvoid main() { n2_FragColor = vec4(1.0); }\n");
            WriteText("materials/blend_" + hint + ".mat", "{\"shader\": \"../shaders/blend_" + hint + ".n2shader\"}");
        }
        WriteText("materials/blend_override.mat",
                  "{\"shader\": \"../shaders/blend_multiply.n2shader\", \"blendMode\": \"additive\", \"alphaMode\": \"opaque\"}");

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "ShaderMatAssetTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    /// The shader's cooked payload where the engine looks for it: <project>/.import/shaders/<uuid>/
    static void Cook(const Shader &shader, const std::string &fixture)
    {
        const fs::path destination = IO::ResourceLoader::Instance().GetMetadataRoot() / "shaders" / shader.GetUUID().ToString();
        fs::create_directories(destination);
        fs::copy(FixtureDir() / fixture, destination, fs::copy_options::overwrite_existing | fs::copy_options::recursive);
    }
};

TEST_F(ShaderMatAssetTest, AShaderIsAProjectAssetOfTypeShader)
{
    const auto shader = IO::Resources::Instance().Load<Shader>(fs::path("res://shaders/solid.n2shader"));
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->GetResourceType(), "Shader");
    EXPECT_EQ(shader->GetName(), "FixtureSolid");
    EXPECT_FALSE(shader->GetUUID() == Math::UUID::ZERO);
    const auto *meta = IO::ResourceLoader::Instance().GetMetadata(shader->GetUUID());
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->resourceType, "Shader");
    // A second load is the cached asset
    EXPECT_EQ(IO::Resources::Instance().Load<Shader>(fs::path("res://shaders/solid.n2shader")), shader);
}

TEST_F(ShaderMatAssetTest, AMatFileNamesTheShaderAndTheValuesOfItsParameters)
{
    WarningCapture capture;
    const auto material = IO::Resources::Instance().Load<Material>(fs::path("res://materials/green.mat"));
    ASSERT_NE(material, nullptr);
    ASSERT_NE(material->GetShader(), nullptr);
    EXPECT_EQ(material->GetShader()->GetName(), "FixtureSolid");
    EXPECT_TRUE(capture.messages.empty()) << (capture.messages.empty() ? "" : capture.messages[0]);
    EXPECT_EQ(material->GetParam("u_Color")->value, (std::array<float, 4>{0, 1, 0, 1}));
    EXPECT_EQ(material->GetParam("u_Gain")->value[0], 2.0f);
    EXPECT_EQ(material->GetParam("u_Offset")->value[2], 0.3f);
    EXPECT_EQ(material->GetParam("u_Flag")->value[0], 1.0f);
    EXPECT_EQ(material->GetParam("u_Steps")->value[0], 4.0f) << "an int is truncated";
    EXPECT_EQ(material->GetParam("u_Pair")->value[1], 4.0f);

    // By a res:// path too
    const auto byPath = IO::Resources::Instance().Load<Material>(fs::path("res://materials/byResPath.mat"));
    ASSERT_NE(byPath, nullptr);
    EXPECT_EQ(byPath->GetShader(), material->GetShader()) << "the same cached shader asset";
    EXPECT_EQ(byPath->ToJson().at("shader"), "res://shaders/solid.n2shader");

    // Written back, it reads the same
    const json saved = material->ToJson();
    EXPECT_EQ(saved.at("shader"), "res://shaders/solid.n2shader");
    EXPECT_EQ(saved.at("params").at("u_Gain"), 2.0f);
    EXPECT_EQ(saved.at("params").at("u_Flag"), true);
    EXPECT_EQ(saved.at("params").at("u_Steps"), 4);
    const auto reloaded = Material::FromJson(saved);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->ToJson(), saved);
}

TEST_F(ShaderMatAssetTest, ParametersTheShaderDoesNotDeclareOrOfTheWrongTypeAreIgnoredWithAWarning)
{
    WarningCapture capture;
    const auto material = IO::Resources::Instance().Load<Material>(fs::path("res://materials/bad.mat"));
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(capture.Mentions("\"nope\""));
    EXPECT_TRUE(capture.Mentions("u_Gain"));
    EXPECT_TRUE(capture.Mentions("u_Offset"));
    EXPECT_TRUE(capture.Mentions("u_Flag"));
    EXPECT_TRUE(capture.Mentions("u_Color"));
    // What was valid stays
    EXPECT_EQ(material->GetParam("u_Pair")->value[1], 2.0f);
    // What was not takes the shader's default (nothing is stored)
    EXPECT_EQ(material->GetParams().count("u_Gain"), 0u);
    EXPECT_EQ(material->GetParam("u_Gain")->value[0], 1.0f);
    EXPECT_EQ(material->GetParams().size(), 1u);

    WarningCapture orphan;
    const auto noShader = IO::Resources::Instance().Load<Material>(fs::path("res://materials/orphan.mat"));
    ASSERT_NE(noShader, nullptr);
    EXPECT_TRUE(orphan.Mentions("params"));
    EXPECT_EQ(noShader->GetShader(), nullptr);

    WarningCapture missing;
    const auto lost = IO::Resources::Instance().Load<Material>(fs::path("res://materials/missing.mat"));
    ASSERT_NE(lost, nullptr);
    EXPECT_TRUE(missing.Mentions("can't load shader"));
    EXPECT_EQ(lost->GetShader(), nullptr);
    EXPECT_TRUE(lost->IsDoubleSided()) << "the rest of the file still reads";
}

TEST_F(ShaderMatAssetTest, ThePayloadIsFoundUnderTheProjectsImportFolderByTheShadersUuid)
{
    const auto shader = IO::Resources::Instance().Load<Shader>(fs::path("res://shaders/solid.n2shader"));
    ASSERT_NE(shader, nullptr);
    std::string error;
    EXPECT_FALSE(shader->MakeBlob(UserShaderTarget::Vulkan, error).has_value()) << "not cooked yet";
    EXPECT_NE(error.find((IO::ResourceLoader::Instance().GetMetadataRoot() / "shaders").string()), std::string::npos) << error;
    Cook(*shader, "solid");
    EXPECT_EQ(shader->GetCookedDirectory(), IO::ResourceLoader::Instance().GetMetadataRoot() / "shaders" / shader->GetUUID().ToString());
    const auto blob = shader->MakeBlob(UserShaderTarget::Vulkan, error);
    ASSERT_TRUE(blob.has_value()) << error;
    EXPECT_FALSE(blob->fragmentSpirv.empty());
}

TEST_F(ShaderMatAssetTest, AUserShaderMaterialReachesTheBackendAsTheCookedBlobWithItsParametersAndNoStandardUniform)
{
    const auto material = IO::Resources::Instance().Load<Material>(fs::path("res://materials/green.mat"));
    ASSERT_NE(material, nullptr);
    Cook(*material->GetShader(), "solid");

    UserShaderRenderer renderer;
    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    ASSERT_EQ(renderer.blobs.size(), 1u);
    EXPECT_EQ(renderer.blobs[0].name, "FixtureSolid");
    EXPECT_FALSE(renderer.blobs[0].fragmentSpirv.empty()) << "the Vulkan payload for a Vulkan target";
    EXPECT_TRUE(renderer.blobs[0].fragmentWgsl.empty());
    ASSERT_EQ(renderer.userMaterials.size(), 1u);
    FakeUserMaterial &gpu = *renderer.userMaterials[0];
    EXPECT_EQ(handle.GetMaterial(), &gpu);
    EXPECT_EQ(renderer.materials.size(), 0u) << "no standard material was made";

    // ApplyUniforms of a user material: the declared parameters (the .mat's values), none of the standard names
    EXPECT_EQ(gpu.vecs["u_Color"], (std::array<float, 4>{0, 1, 0, 1}));
    EXPECT_EQ(gpu.floats["u_Gain"], 2.0f);
    EXPECT_EQ(gpu.ints["u_Flag"], 1);
    EXPECT_EQ(gpu.ints["u_Steps"], 4);
    EXPECT_EQ(gpu.vecs["u_Pair"], (std::array<float, 4>{3, 4, 0, 0}));
    for (const char *standard : {"uAlbedo", "uHasTexture", "uAlphaCutoff", "uSaturation", "uFlash", "uSmoothness", "uMetallic"})
    {
        EXPECT_EQ(gpu.names.count(standard), 0u) << standard << " is not a name of a user shader";
    }
    EXPECT_EQ(gpu.names.size(), 6u);

    // The same cached GPU material for the next acquire; another renderer target takes its own payload
    GpuCache::Handle again = GpuCache::AcquireMaterial(renderer, material);
    EXPECT_EQ(again.GetMaterial(), &gpu);
    EXPECT_EQ(renderer.blobs.size(), 1u);
    UserShaderRenderer web;
    web.target = UserShaderTarget::WebGpu;
    GpuCache::Handle onWeb = GpuCache::AcquireMaterial(web, material);
    ASSERT_TRUE(onWeb);
    ASSERT_EQ(web.blobs.size(), 1u);
    EXPECT_FALSE(web.blobs[0].fragmentWgsl.empty());
    EXPECT_TRUE(web.blobs[0].fragmentSpirv.empty());
    UserShaderRenderer gl;
    gl.target = UserShaderTarget::OpenGl;
    GpuCache::Handle onGl = GpuCache::AcquireMaterial(gl, material);
    ASSERT_TRUE(onGl);
    EXPECT_EQ(gl.blobs[0].fragmentGlsl.rfind("#version 330 core", 0), 0u);

    // Releasing every share destroys the renderer's material first, then its shader
    handle.Release(true);
    again.Release(true);
    ASSERT_GE(renderer.events.size(), 4u);
    EXPECT_EQ(renderer.events[2], "destroyMaterial");
    EXPECT_EQ(renderer.events[3], "destroyShader");
    onWeb.Release(true);
    onGl.Release(true);
}

TEST_F(ShaderMatAssetTest, TheReservedBaseColorAndBaseColorTextureFeedAShaderThatDeclaresThemAndTexturesGoToTheirSlots)
{
    const auto shader = IO::Resources::Instance().Load<Shader>(fs::path("res://shaders/reserved.n2shader"));
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->FindParam("baseColorTexture")->slot, 0);
    EXPECT_EQ(shader->FindParam("u_Mask")->slot, 1);

    // No cooked data for this shader: borrow the "textured" manifest shape by pointing at a folder we write
    // (three params, but the engine only needs the manifest to match the header): cook it by hand from the textured fixture
    const fs::path cooked = s_root / "cooked";
    fs::create_directories(cooked);
    json manifest = json::parse(ReadFile(FixtureDir() / "solid" / "manifest.json"));
    manifest["name"] = "Reserved";
    manifest["blockSize"] = 16;
    manifest["params"] = json::array({
        {{"name", "baseColor"}, {"type", "color"}, {"offset", 0}, {"size", 16}, {"default", {1, 1, 1, 1}}},
        {{"name", "u_Mask"}, {"type", "texture"}, {"slot", 1}, {"default", "black"}},
        {{"name", "baseColorTexture"}, {"type", "texture"}, {"slot", 0}, {"default", "white"}},
    });
    std::ofstream(cooked / "manifest.json") << manifest.dump(2);
    fs::copy_file(FixtureDir() / "solid" / manifest["payloads"]["vulkan"]["file"].get<std::string>(),
                  cooked / manifest["payloads"]["vulkan"]["file"].get<std::string>(), fs::copy_options::overwrite_existing);
    shader->SetCookedDirectory(cooked);

    auto material = Material::Create(ShadingModel::Lit);
    material->SetShader(shader);
    material->SetBaseColor(Common::Color(0.5f, 0.25f, 1.0f, 1.0f));
    const auto baseTexture = OnePixel(40);
    const auto maskTexture = OnePixel(50);
    material->SetBaseColorTexture(baseTexture);
    EXPECT_TRUE(material->SetTexture("u_Mask", maskTexture));

    UserShaderRenderer renderer;
    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle) << "the payload is not what the shader says, but the engine only checks the header's shape";
    ASSERT_EQ(renderer.userMaterials.size(), 1u);
    FakeUserMaterial &gpu = *renderer.userMaterials[0];
    ASSERT_EQ(renderer.textures.size(), 2u);
    EXPECT_NE(gpu.slots[0], nullptr) << "the base colour texture, in slot 0";
    EXPECT_NE(gpu.slots[1], nullptr) << "u_Mask, in slot 1";
    EXPECT_NE(gpu.slots[0], gpu.slots[1]);
    EXPECT_EQ(gpu.slots[2], nullptr);

    // The base colour parameter: the material's own, times the draw's tint
    material->ApplyUniforms(gpu, Common::Color(1.0f, 0.5f, 1.0f, 0.5f));
    EXPECT_EQ(gpu.vecs["baseColor"], (std::array<float, 4>{0.5f, 0.125f, 1.0f, 0.5f}));
    // A value the .mat sets wins over the material's base colour (the tint still applies)
    EXPECT_TRUE(material->SetColor("baseColor", Common::Color(0.0f, 1.0f, 0.0f, 1.0f)));
    material->ApplyUniforms(gpu, Common::Color(1.0f, 0.5f, 1.0f, 1.0f));
    EXPECT_EQ(gpu.vecs["baseColor"], (std::array<float, 4>{0.0f, 0.5f, 0.0f, 1.0f}));
    handle.Release(true);
}

// ============================================================================
// The fallback, and built-in materials untouched
// ============================================================================

TEST(UserShaderFallbackTest, ABackendWithoutUserShadersDrawsTheUnlitFallbackAndLogsOnce)
{
    auto material = ShaderMaterial("solid");
    material->SetBaseColor(Common::Color(0.2f, 0.4f, 0.6f, 1.0f));
    const auto texture = OnePixel(77);
    material->SetBaseColorTexture(texture);

    UserShaderRenderer renderer;
    renderer.target = std::nullopt; // the software renderer's answer
    WarningCapture capture;
    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_TRUE(renderer.blobs.empty());
    EXPECT_TRUE(renderer.userMaterials.empty());
    ASSERT_EQ(renderer.materials.size(), 1u) << "a standard material";
    EXPECT_EQ(renderer.materials[0]->GetShader(), renderer.GetStandardUnlitShader());
    EXPECT_NE(renderer.materials[0]->GetTexture(), nullptr) << "with the base colour texture";

    material->ApplyUniforms(*handle.GetMaterial());
    const auto *sw = RecordingMeshRenderer::AsSW(handle.GetMaterial());
    ASSERT_NE(sw, nullptr);
    const auto albedo = sw->GetVec4("uAlbedo", {0, 0, 0, 0});
    EXPECT_FLOAT_EQ(albedo[0], 0.2f);
    EXPECT_FLOAT_EQ(albedo[2], 0.6f);
    ASSERT_EQ(capture.messages.size(), 1u);
    EXPECT_NE(capture.messages[0].find("cannot draw user shaders"), std::string::npos);
    EXPECT_NE(capture.messages[0].find("FixtureSolid"), std::string::npos) << "it names the shader (a loaded one, its file)";

    // A second material of the same shader does not log again
    auto other = Material::Create();
    other->SetShader(material->GetShader());
    GpuCache::Handle second = GpuCache::AcquireMaterial(renderer, other);
    EXPECT_EQ(capture.messages.size(), 1u) << "once per shader";
    (void)second;
    handle.Release(true);
}

TEST(UserShaderFallbackTest, ShaderThatIsNotCookedStaleOrRefusedByTheRendererGetsTheFallbackToo)
{
    const std::string solidSource = FixtureSource("solid");
    {
        // Not cooked
        auto material = Material::Create();
        auto shader = Shader::FromSource(solidSource, "uncooked.n2shader");
        shader->SetCookedDirectory(FixtureDir() / "nowhere");
        material->SetShader(shader);
        UserShaderRenderer renderer;
        WarningCapture capture;
        GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
        ASSERT_TRUE(handle);
        EXPECT_EQ(renderer.materials.size(), 1u);
        ASSERT_EQ(capture.messages.size(), 1u);
        EXPECT_NE(capture.messages[0].find("FixtureSolid"), std::string::npos);
        EXPECT_NE(capture.messages[0].find("no cooked data"), std::string::npos);
        handle.Release(true);
    }
    {
        // The renderer refuses the shader (a compile error)
        auto material = ShaderMaterial("solid");
        UserShaderRenderer renderer;
        renderer.refuseShaders = true;
        WarningCapture capture;
        GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
        ASSERT_TRUE(handle);
        EXPECT_EQ(renderer.blobs.size(), 1u);
        EXPECT_EQ(renderer.materials.size(), 1u) << "the fallback";
        EXPECT_TRUE(capture.Mentions("refused"));
        handle.Release(true);
        EXPECT_TRUE(renderer.userShaders.empty());
    }
    {
        // The shader was made but the renderer cannot make a material of it: the shader is destroyed again, not leaked
        auto material = ShaderMaterial("solid");
        UserShaderRenderer renderer;
        renderer.refuseMaterials = true;
        GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
        ASSERT_TRUE(handle);
        EXPECT_EQ(renderer.materials.size(), 1u);
        EXPECT_TRUE(renderer.userShaders.empty()) << "no user shader is left on the renderer";
        handle.Release(true);
    }
}

TEST(UserShaderFallbackTest, TheEditorStyleIsFlatMagentaWithNoTexture)
{
    struct Restore
    {
        ~Restore() { Rendering::RenderSettings::SetUserShaderErrorStyle(UserShaderErrorStyle::Unlit); }
    } restore;
    Rendering::RenderSettings::SetUserShaderErrorStyle(UserShaderErrorStyle::Magenta);

    auto material = ShaderMaterial("solid");
    material->SetBaseColorTexture(OnePixel(9));
    UserShaderRenderer renderer;
    renderer.target = std::nullopt;
    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_EQ(renderer.materials[0]->GetTexture(), nullptr) << "a texture would tint the magenta";
    material->ApplyUniforms(*handle.GetMaterial());
    const auto *sw = RecordingMeshRenderer::AsSW(handle.GetMaterial());
    const auto albedo = sw->GetVec4("uAlbedo", {0, 0, 0, 0});
    EXPECT_EQ(albedo, (std::array<float, 4>{1, 0, 1, 1}));
    handle.Release(true);
}

TEST(UserShaderFallbackTest, ABuiltInMaterialNeverTouchesTheUserShaderPath)
{
    for (const ShadingModel shading : {ShadingModel::Unlit, ShadingModel::Lit, ShadingModel::Pbr})
    {
        auto material = Material::Create(shading);
        material->SetBaseColorTexture(OnePixel(5));
        UserShaderRenderer renderer;
        GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
        ASSERT_TRUE(handle);
        EXPECT_TRUE(renderer.blobs.empty());
        EXPECT_TRUE(renderer.userMaterials.empty());
        EXPECT_TRUE(renderer.userShaders.empty());
        ASSERT_EQ(renderer.materials.size(), 1u);
        EXPECT_EQ(renderer.materials[0]->GetShader(),
                  shading == ShadingModel::Unlit ? renderer.GetStandardUnlitShader() : renderer.GetStandardLitShader());
        // The key a built-in material is cached under is what it always was: its GPU version (no shader bits)
        EXPECT_EQ(GpuCache::MaterialVersion(*material), material->GetGpuVersion());
        material->ApplyUniforms(*handle.GetMaterial());
        const auto *sw = RecordingMeshRenderer::AsSW(handle.GetMaterial());
        EXPECT_TRUE(sw->GetVec4("uAlbedo", {0, 0, 0, 0})[0] == 1.0f);
        handle.Release(true);
        EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 1);
    }
}

TEST(UserShaderFallbackTest, AReloadedShaderMakesTheMaterialsThatUseItNewGpuMaterials)
{
    auto shader = FixtureShader("solid");
    auto material = Material::Create();
    material->SetShader(shader);
    const std::uint64_t before = GpuCache::MaterialVersion(*material);
    const fs::path file = fs::temp_directory_path() / "n2engine_reload.n2shader";
    std::ofstream(file, std::ios::binary) << FixtureSource("solid");
    ASSERT_TRUE(shader->Load(file));
    EXPECT_NE(GpuCache::MaterialVersion(*material), before) << "a Load bumps the shader's version, which the key carries";
    std::error_code ec;
    fs::remove(file, ec);
}

// ============================================================================
// .mat to pixels on the software renderer (the fallback, whole path through Scene::Render)
// ============================================================================

namespace
{
    constexpr int Size = 64;

    Camera CameraAt(const Math::Vector3 &position)
    {
        Camera camera;
        camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(position);
        camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
        return camera;
    }

    std::vector<std::uint8_t> RenderQuad(const std::shared_ptr<Material> &material)
    {
        Renderer::Software::SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Size) * Size * 4);
        {
            const Camera camera = CameraAt(Math::Vector3(0.0f, 0.0f, 2.0f));
            auto scene = Scene::Create("UserShaderSoftwareGolden");
            auto gameObject = GameObject::Create("Quad");
            auto *meshRenderer = gameObject->AddComponent<Rendering::MeshRenderer>();
            meshRenderer->SetMesh(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Quad));
            meshRenderer->SetMaterial(0, material);
            scene->AddRootGameObject(gameObject);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(pixels.data(), Size, Size);
            scene.reset();
        }
        renderer.Shutdown();
        return pixels;
    }

    std::array<std::uint8_t, 3> Centre(const std::vector<std::uint8_t> &pixels)
    {
        const std::size_t i = (static_cast<std::size_t>(Size / 2) * Size + Size / 2) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2]};
    }
}

TEST(UserShaderSoftwareGoldenTest, AMaterialWithAShaderDrawsItsBaseColourUnlitOnTheSoftwareRendererWithOneWarning)
{
    auto material = ShaderMaterial("solid"); // the shader would draw red; the software renderer cannot run it
    material->SetBaseColor(Common::Color(0.0f, 1.0f, 0.0f, 1.0f));
    WarningCapture capture;
    const auto pixels = RenderQuad(material);
    EXPECT_EQ(Centre(pixels), (std::array<std::uint8_t, 3>{0, 255, 0})) << "the base colour, unlit: visible and obviously not the shader's";
    EXPECT_EQ(std::ranges::count_if(capture.messages, [](const std::string &m) { return m.find("cannot draw user shaders") != std::string::npos; }), 1);
    // Drawn again: the cached fallback, no new warning
    RenderQuad(material);
    EXPECT_EQ(std::ranges::count_if(capture.messages, [](const std::string &m) { return m.find("cannot draw user shaders") != std::string::npos; }), 1);

    // The same material with the editor's style: flat magenta
    Rendering::RenderSettings::SetUserShaderErrorStyle(UserShaderErrorStyle::Magenta);
    const auto magenta = RenderQuad(ShaderMaterial("solid"));
    Rendering::RenderSettings::SetUserShaderErrorStyle(UserShaderErrorStyle::Unlit);
    EXPECT_EQ(Centre(magenta), (std::array<std::uint8_t, 3>{255, 0, 255}));
}

// ===== Stage 9a: premultiplied and multiply blending =====

TEST(BlendModeTest, TheBlendFunctionOfEachModeAndAdditiveWinning)
{
    using namespace Renderer::Common;
    const BlendFunc alpha = BlendFuncFor(false, BlendVariant::Alpha);
    EXPECT_EQ(alpha.srcColor, BlendFactor::SrcAlpha);
    EXPECT_EQ(alpha.dstColor, BlendFactor::OneMinusSrcAlpha);
    EXPECT_EQ(alpha.srcAlpha, BlendFactor::SrcAlpha);
    EXPECT_EQ(alpha.dstAlpha, BlendFactor::OneMinusSrcAlpha);
    const BlendFunc premultiplied = BlendFuncFor(false, BlendVariant::Premultiplied);
    EXPECT_EQ(premultiplied.srcColor, BlendFactor::One);
    EXPECT_EQ(premultiplied.dstColor, BlendFactor::OneMinusSrcAlpha);
    EXPECT_EQ(premultiplied.srcAlpha, BlendFactor::One);
    EXPECT_EQ(premultiplied.dstAlpha, BlendFactor::OneMinusSrcAlpha);
    const BlendFunc multiply = BlendFuncFor(false, BlendVariant::Multiply);
    EXPECT_EQ(multiply.srcColor, BlendFactor::DstColor);
    EXPECT_EQ(multiply.dstColor, BlendFactor::Zero);
    EXPECT_EQ(multiply.srcAlpha, BlendFactor::Zero) << "the target's alpha stays";
    EXPECT_EQ(multiply.dstAlpha, BlendFactor::One);
    for (const BlendVariant variant : {BlendVariant::Alpha, BlendVariant::Premultiplied, BlendVariant::Multiply})
    {
        const BlendFunc additive = BlendFuncFor(true, variant);
        EXPECT_EQ(additive.srcColor, BlendFactor::SrcAlpha);
        EXPECT_EQ(additive.dstColor, BlendFactor::One);
        EXPECT_EQ(additive.dstAlpha, BlendFactor::One);
    }
    EXPECT_EQ(RenderState{}.variant, BlendVariant::Alpha);
}

TEST(BlendModeTest, AMaterialsBlendModeIsAMatKeyThatIsWrittenOnlyWhenItIsNotAlpha)
{
    auto material = Material::Create(ShadingModel::Unlit);
    EXPECT_EQ(material->GetBlendMode(), Rendering::BlendMode::Alpha);
    EXPECT_FALSE(material->ToJson().contains("blendMode")) << "an old .mat round-trips byte for byte";
    for (const auto &[mode, name] : {std::pair{Rendering::BlendMode::Additive, "additive"},
                                     std::pair{Rendering::BlendMode::Premultiplied, "premultiplied"},
                                     std::pair{Rendering::BlendMode::Multiply, "multiply"}})
    {
        material->SetBlendMode(mode);
        const json saved = material->ToJson();
        EXPECT_EQ(saved.at("blendMode"), name);
        WarningCapture capture;
        const auto loaded = Material::FromJson(saved);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->GetBlendMode(), mode);
        EXPECT_TRUE(capture.messages.empty());
    }
    WarningCapture capture;
    const auto bad = Material::FromJson(json{{"blendMode", "screen"}});
    ASSERT_NE(bad, nullptr);
    EXPECT_EQ(bad->GetBlendMode(), Rendering::BlendMode::Alpha);
    EXPECT_FALSE(capture.messages.empty());
}

TEST(BlendModeTest, ADrawStateTakesTheMaterialsModeOnlyForABlendMaterialInABlendingState)
{
    using Renderer::Common::BlendVariant;
    using Renderer::Common::RenderState;
    auto material = Material::Create(ShadingModel::Unlit);
    material->SetAlphaMode(Rendering::AlphaMode::Blend);
    material->SetBlendMode(Rendering::BlendMode::Multiply);
    RenderState state = RenderState::Transparent();
    Rendering::MeshDrawing::ApplyBlendMode(state, *material);
    EXPECT_EQ(state.variant, BlendVariant::Multiply);
    EXPECT_FALSE(state.additive);
    material->SetBlendMode(Rendering::BlendMode::Additive);
    Rendering::MeshDrawing::ApplyBlendMode(state, *material);
    EXPECT_TRUE(state.additive);
    material->SetBlendMode(Rendering::BlendMode::Premultiplied);
    Rendering::MeshDrawing::ApplyBlendMode(state, *material);
    EXPECT_FALSE(state.additive);
    EXPECT_EQ(state.variant, BlendVariant::Premultiplied);

    RenderState opaque = RenderState::Opaque();
    Rendering::MeshDrawing::ApplyBlendMode(opaque, *material);
    EXPECT_EQ(opaque, RenderState::Opaque()) << "an opaque state is not touched";
    material->SetAlphaMode(Rendering::AlphaMode::Opaque);
    RenderState transparent = RenderState::Transparent();
    Rendering::MeshDrawing::ApplyBlendMode(transparent, *material);
    EXPECT_EQ(transparent, RenderState::Transparent()) << "a material that is not Blend is not touched";
}

TEST_F(ShaderMatAssetTest, AShadersBlendHintSetsTheMaterialsModeUnlessTheMatSaysOtherwise)
{
    const auto load = [](const std::string &name)
    { return IO::Resources::Instance().Load<Material>(fs::path("res://materials/" + name + ".mat")); };

    const auto premultiplied = load("blend_premultiplied");
    ASSERT_NE(premultiplied, nullptr);
    EXPECT_EQ(premultiplied->GetBlendMode(), Rendering::BlendMode::Premultiplied);
    EXPECT_TRUE(premultiplied->IsBlended()) << "the hint makes the material a Blend one, as alpha always did";
    EXPECT_EQ(load("blend_multiply")->GetBlendMode(), Rendering::BlendMode::Multiply);
    EXPECT_EQ(load("blend_additive")->GetBlendMode(), Rendering::BlendMode::Additive);
    EXPECT_EQ(load("blend_alpha")->GetBlendMode(), Rendering::BlendMode::Alpha);
    // A value the .mat sets wins over the hint
    const auto overridden = load("blend_override");
    ASSERT_NE(overridden, nullptr);
    EXPECT_EQ(overridden->GetBlendMode(), Rendering::BlendMode::Additive);
    EXPECT_FALSE(overridden->IsBlended());

    // The private Material of a component's shader slot follows the hint too
    Rendering::UserShaderSlot slot;
    slot.SetShader(premultiplied->GetShader());
    ASSERT_NE(slot.GetMaterial(), nullptr);
    EXPECT_EQ(slot.GetMaterial()->GetBlendMode(), Rendering::BlendMode::Premultiplied);
    slot.SetShader(load("blend_alpha")->GetShader());
    EXPECT_EQ(slot.GetMaterial()->GetBlendMode(), Rendering::BlendMode::Alpha);
}
