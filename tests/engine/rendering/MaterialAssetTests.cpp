#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

#include "engine/io/AssetMetadata.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Texture.hpp"

#include "MeshTestSupport.hpp"
#include "TextureTestSupport.hpp"

// The Material asset: defaults, .mat JSON (every key, warnings for bad values), .mat files as project assets with
// their textures, ToJson, versions and the uniforms it sets on a GPU material

using namespace N2Engine;
using Rendering::AlphaMode;
using Rendering::Material;
using Rendering::ShadingModel;
using Rendering::Texture;
using MeshTestSupport::WarningCapture;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    void ExpectColor(const Common::Color &actual, const float r, const float g, const float b, const float a)
    {
        EXPECT_FLOAT_EQ(actual.r, r);
        EXPECT_FLOAT_EQ(actual.g, g);
        EXPECT_FLOAT_EQ(actual.b, b);
        EXPECT_FLOAT_EQ(actual.a, a);
    }

    void ExpectDefaults(const Material &material)
    {
        EXPECT_EQ(material.GetShading(), ShadingModel::Lit);
        ExpectColor(material.GetBaseColor(), 1.0f, 1.0f, 1.0f, 1.0f);
        EXPECT_EQ(material.GetBaseColorTexture(), nullptr);
        EXPECT_EQ(material.GetAlphaMode(), AlphaMode::Opaque);
        EXPECT_FLOAT_EQ(material.GetAlphaCutoff(), 0.5f);
        EXPECT_FALSE(material.IsDoubleSided());
        EXPECT_FLOAT_EQ(material.GetSmoothness(), 0.5f);
        EXPECT_FLOAT_EQ(material.GetMetallic(), 0.0f);
        ExpectColor(material.GetEmissive(), 0.0f, 0.0f, 0.0f, 1.0f);
        EXPECT_EQ(material.GetNormalTexture(), nullptr);
        EXPECT_EQ(material.GetOcclusionTexture(), nullptr);
        EXPECT_FLOAT_EQ(material.GetOcclusionStrength(), 1.0f);
        EXPECT_EQ(material.GetMetallicRoughnessTexture(), nullptr);
        EXPECT_EQ(material.GetEmissiveTexture(), nullptr);
    }
}

// ============================================================================
// Defaults and runtime materials
// ============================================================================

TEST(MaterialTest, ANewMaterialIsLitOpaqueWhite)
{
    const auto material = Material::Create();
    ASSERT_NE(material, nullptr);
    ExpectDefaults(*material);
    EXPECT_EQ(material->GetResourceType(), "Material");
    EXPECT_FALSE(material->IsBlended());
    EXPECT_EQ(Material::Create(ShadingModel::Unlit)->GetShading(), ShadingModel::Unlit);
}

TEST(MaterialTest, TheSharedDefaultsAreLitAndUnlitWhite)
{
    const auto lit = Material::GetDefault();
    const auto unlit = Material::GetDefaultUnlit();
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(unlit, nullptr);
    EXPECT_EQ(lit, Material::GetDefault()) << "one shared material";
    EXPECT_EQ(lit->GetShading(), ShadingModel::Lit);
    EXPECT_EQ(unlit->GetShading(), ShadingModel::Unlit);
    ExpectColor(lit->GetBaseColor(), 1.0f, 1.0f, 1.0f, 1.0f);
    ExpectColor(unlit->GetBaseColor(), 1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_EQ(lit->GetAlphaMode(), AlphaMode::Opaque);
    EXPECT_NE(lit->GetUUID(), unlit->GetUUID());
}

TEST(MaterialTest, EveryChangeBumpsTheVersionAndValuesAreClamped)
{
    const auto material = Material::Create();
    std::uint64_t version = material->GetVersion();
    const auto changed = [&]
    {
        const bool bumped = material->GetVersion() > version;
        version = material->GetVersion();
        return bumped;
    };

    material->SetBaseColor(Common::Color{0.5f, 0.25f, 1.0f, 1.0f});
    EXPECT_TRUE(changed());
    material->SetShading(ShadingModel::Unlit);
    EXPECT_TRUE(changed());
    material->SetAlphaMode(AlphaMode::Blend);
    EXPECT_TRUE(changed());
    EXPECT_TRUE(material->IsBlended());
    material->SetAlphaCutoff(2.0f);
    EXPECT_TRUE(changed());
    EXPECT_FLOAT_EQ(material->GetAlphaCutoff(), 1.0f);
    material->SetSmoothness(-1.0f);
    EXPECT_TRUE(changed());
    EXPECT_FLOAT_EQ(material->GetSmoothness(), 0.0f);
    material->SetDoubleSided(true);
    EXPECT_TRUE(changed());
    material->SetMetallic(0.75f);
    EXPECT_TRUE(changed());
    material->SetBaseColorTexture(Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_TRUE(changed());
}

TEST(MaterialTest, OnlyTheShaderAndTextureBumpTheGpuVersion)
{
    const auto material = Material::Create();
    EXPECT_EQ(material->GetVersion(), 1u);
    EXPECT_EQ(material->GetGpuVersion(), 1u);
    material->SetBaseColor(Common::Color::Red);
    material->SetAlphaMode(AlphaMode::Blend);
    material->SetAlphaCutoff(0.2f);
    material->SetDoubleSided(true);
    material->SetSmoothness(0.9f);
    material->SetMetallic(0.5f);
    material->SetEmissive(Common::Color::Blue);
    material->SetNormalScale(2.0f);
    EXPECT_EQ(material->GetGpuVersion(), 1u) << "uniforms and state, set per draw";
    EXPECT_EQ(material->GetVersion(), 9u);
    material->SetShading(ShadingModel::Unlit);
    EXPECT_EQ(material->GetGpuVersion(), 2u);
    material->SetShading(ShadingModel::Pbr);
    EXPECT_EQ(material->GetGpuVersion(), 3u) << "the Pbr shading is a shading change too";
    material->SetBaseColorTexture(nullptr);
    EXPECT_EQ(material->GetGpuVersion(), 4u);
    material->SetNormalTexture(Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(material->GetGpuVersion(), 5u) << "the normal map is a GPU texture";
    material->SetMetallicRoughnessTexture(Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(material->GetGpuVersion(), 6u) << "so is the metallic-roughness texture";

    // A loaded material starts at 1, as a new one does
    const auto loaded = Material::FromJson(json{{"shading", "unlit"}, {"smoothness", 0.25}});
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetVersion(), 1u);
    EXPECT_EQ(loaded->GetGpuVersion(), 1u);
}

TEST(MaterialTest, ApplyUniformsSetsTheStandardShadersUniforms)
{
    Renderer::Software::SWShader lit(Renderer::Software::SWShaderType::Lit);
    Renderer::Software::SWMaterial gpu(&lit);
    const auto material = Material::Create();
    material->SetBaseColor(Common::Color{0.5f, 1.0f, 0.25f, 0.8f});
    material->SetSmoothness(0.75f);
    material->SetAlphaCutoff(0.3f);

    // The tint multiplies the base colour; no cutoff unless the mode is Mask
    material->ApplyUniforms(gpu, Common::Color{0.5f, 0.5f, 1.0f, 1.0f});
    const auto albedo = gpu.GetVec4("uAlbedo");
    EXPECT_FLOAT_EQ(albedo[0], 0.25f);
    EXPECT_FLOAT_EQ(albedo[1], 0.5f);
    EXPECT_FLOAT_EQ(albedo[2], 0.25f);
    EXPECT_FLOAT_EQ(albedo[3], 0.8f);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uSmoothness"), 0.75f);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uAlphaCutoff", -1.0f), 0.0f);

    material->SetAlphaMode(AlphaMode::Mask);
    material->ApplyUniforms(gpu);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uAlphaCutoff"), 0.3f);
}

TEST(MaterialTest, EmissiveAndOcclusionTexturesMakeANewGpuMaterialButTheirValuesDoNot)
{
    const auto material = Material::Create();
    material->SetEmissive(Common::Color{1.0f, 0.5f, 0.0f, 1.0f});
    material->SetOcclusionStrength(0.25f);
    EXPECT_EQ(material->GetGpuVersion(), 1u) << "the colour and the strength are uniforms";
    EXPECT_FLOAT_EQ(material->GetOcclusionStrength(), 0.25f);

    material->SetEmissiveTexture(Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(material->GetGpuVersion(), 2u) << "a texture is part of what the GPU material is made of";
    material->SetOcclusionTexture(Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(material->GetGpuVersion(), 3u);

    // The strength is clamped to 0..1; a non-finite one is the default
    material->SetOcclusionStrength(3.0f);
    EXPECT_FLOAT_EQ(material->GetOcclusionStrength(), 1.0f);
    material->SetOcclusionStrength(-1.0f);
    EXPECT_FLOAT_EQ(material->GetOcclusionStrength(), 0.0f);
    material->SetOcclusionStrength(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(material->GetOcclusionStrength(), 1.0f);
}

TEST(MaterialTest, ApplyUniformsSetsTheEmissiveAndOcclusionInputsForLitMaterialsOnly)
{
    Renderer::Software::SWShader litShader(Renderer::Software::SWShaderType::Lit);
    Renderer::Software::SWMaterial gpu(&litShader);
    const auto material = Material::Create();
    material->SetEmissive(Common::Color{0.25f, 0.5f, 0.75f, 0.1f});
    material->SetOcclusionStrength(0.5f);

    // No textures: the flags are off
    material->ApplyUniforms(gpu);
    const auto emissive = gpu.GetVec4("uEmissive");
    EXPECT_FLOAT_EQ(emissive[0], 0.25f);
    EXPECT_FLOAT_EQ(emissive[1], 0.5f);
    EXPECT_FLOAT_EQ(emissive[2], 0.75f);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uOcclusionStrength", -1.0f), 0.5f);
    EXPECT_EQ(gpu.GetInt("uHasEmissiveTexture", -1), 0);
    EXPECT_EQ(gpu.GetInt("uHasOcclusionTexture", -1), 0);

    // With textures on the GPU material, the has-texture flags are on
    Renderer::Software::SWTexture glow;
    Renderer::Software::SWTexture occlusion;
    glow.data = {1, 2, 3, 4};
    glow.width = glow.height = 1;
    glow.channels = 4;
    occlusion.data = {1, 2, 3, 4};
    occlusion.width = occlusion.height = 1;
    occlusion.channels = 4;
    gpu.SetAuxTexture(Renderer::Common::AuxTexture::Emissive, &glow);
    gpu.SetAuxTexture(Renderer::Common::AuxTexture::Occlusion, &occlusion);
    Rendering::TextureSettings data;
    data.srgb = false;
    const auto colourTexture = Texture::CreateFromEncoded(TextureTestSupport::MakeBmp(1, 1, {{9, 9, 9}}));
    ASSERT_NE(colourTexture, nullptr);
    ASSERT_TRUE(colourTexture->GetSettings().srgb);
    material->SetBaseColorTexture(colourTexture);
    material->SetEmissiveTexture(Texture::CreateFromEncoded(TextureTestSupport::MakeBmp(1, 1, {{9, 9, 9}}), data));
    material->ApplyUniforms(gpu);
    EXPECT_EQ(gpu.GetInt("uHasEmissiveTexture", -1), 1);
    EXPECT_EQ(gpu.GetInt("uHasOcclusionTexture", -1), 1);

    // An unlit material never reads them: none are set
    Renderer::Software::SWShader unlitShader(Renderer::Software::SWShaderType::Unlit);
    Renderer::Software::SWMaterial unlitGpu(&unlitShader);
    const auto unlit = Material::Create(ShadingModel::Unlit);
    unlit->SetEmissive(Common::Color::White);
    unlit->ApplyUniforms(unlitGpu);
    EXPECT_EQ(unlitGpu.GetInt("uHasEmissiveTexture", -1), -1);
    EXPECT_FLOAT_EQ(unlitGpu.GetVec4("uEmissive", {-1, -1, -1, -1})[0], -1.0f);
}

// ============================================================================
// .mat JSON
// ============================================================================

TEST(MaterialJsonTest, AnEmptyObjectIsTheDefaults)
{
    WarningCapture capture;
    const auto material = Material::FromJson(json::object());
    ASSERT_NE(material, nullptr);
    ExpectDefaults(*material);
    EXPECT_TRUE(capture.messages.empty());
}

TEST(MaterialJsonTest, ReadsEveryKey)
{
    WarningCapture capture;
    const json mat = {
        {"shading", "unlit"},
        {"baseColor", {{"r", 0.5}, {"g", 0.25}, {"b", 1.0}, {"a", 0.75}}},
        {"baseColorTexture", nullptr},
        {"alphaMode", "MASK"}, // glTF's spelling works too
        {"alphaCutoff", 0.4},
        {"doubleSided", true},
        {"smoothness", 0.9},
        {"metallic", 0.6},
        {"emissive", json::array({0.1, 0.2, 0.3})},
        {"normalTexture", nullptr},
        {"occlusionTexture", nullptr},
        {"metallicRoughnessTexture", nullptr},
        {"emissiveTexture", nullptr},
    };
    const auto material = Material::FromJson(mat, {}, "every_key");
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(capture.messages.empty()) << capture.messages.front();
    EXPECT_EQ(material->GetShading(), ShadingModel::Unlit);
    ExpectColor(material->GetBaseColor(), 0.5f, 0.25f, 1.0f, 0.75f);
    EXPECT_EQ(material->GetAlphaMode(), AlphaMode::Mask);
    EXPECT_FLOAT_EQ(material->GetAlphaCutoff(), 0.4f);
    EXPECT_TRUE(material->IsDoubleSided());
    EXPECT_FLOAT_EQ(material->GetSmoothness(), 0.9f);
    EXPECT_FLOAT_EQ(material->GetMetallic(), 0.6f);
    ExpectColor(material->GetEmissive(), 0.1f, 0.2f, 0.3f, 1.0f);

    EXPECT_EQ(Material::FromJson(json{{"alphaMode", "blend"}})->GetAlphaMode(), AlphaMode::Blend);
    EXPECT_EQ(Material::FromJson(json{{"alphaMode", "opaque"}})->GetAlphaMode(), AlphaMode::Opaque);
    EXPECT_EQ(Material::FromJson(json{{"shading", "lit"}})->GetShading(), ShadingModel::Lit);
}

TEST(MaterialJsonTest, BadValuesAreIgnoredWithAWarningAndKeepTheDefaults)
{
    WarningCapture capture;
    const json mat = {
        {"shading", "pbr"},
        {"baseColor", "red"},
        {"alphaMode", 3},
        {"alphaCutoff", "half"},
        {"doubleSided", "yes"},
        {"smoothness", json::array()},
        {"emissive", {{"r", 1}, {"q", 2}}},
        {"colour", {1, 0, 0}},
    };
    const auto material = Material::FromJson(mat, {}, "bad.mat");
    ASSERT_NE(material, nullptr);
    ExpectDefaults(*material);
    EXPECT_EQ(capture.messages.size(), mat.size()) << "one warning per bad key";
    EXPECT_TRUE(capture.Mentions("bad.mat"));
    EXPECT_TRUE(capture.Mentions("\"shading\""));
    EXPECT_TRUE(capture.Mentions("unknown key \"colour\""));

    // Out of range: clamped, with a warning
    WarningCapture rangeCapture;
    const auto clamped = Material::FromJson(json{{"alphaCutoff", 1.5}, {"smoothness", -0.5}});
    EXPECT_FLOAT_EQ(clamped->GetAlphaCutoff(), 1.0f);
    EXPECT_FLOAT_EQ(clamped->GetSmoothness(), 0.0f);
    EXPECT_EQ(rangeCapture.messages.size(), 2u);

    // Not an object at all
    WarningCapture errorCapture;
    EXPECT_EQ(Material::FromJson(json::array()), nullptr);
    EXPECT_EQ(errorCapture.messages.size(), 1u);
}

TEST(MaterialJsonTest, OldMaterialFilesWithoutTheNewKeysLoadWithTheirDefaults)
{
    // A .mat written before occlusionStrength existed: the keys it has are read, the strength is 1 and nothing warns
    WarningCapture capture;
    const json old = {
        {"shading", "lit"},
        {"baseColor", {{"r", 1}, {"g", 0.5}, {"b", 0.5}, {"a", 1}}},
        {"baseColorTexture", nullptr},
        {"alphaMode", "opaque"},
        {"alphaCutoff", 0.5},
        {"doubleSided", false},
        {"smoothness", 0.5},
        {"metallic", 0.0},
        {"emissive", {{"r", 0}, {"g", 0}, {"b", 0}, {"a", 1}}},
        {"normalTexture", nullptr},
        {"occlusionTexture", nullptr},
        {"metallicRoughnessTexture", nullptr},
        {"emissiveTexture", nullptr},
    };
    const auto material = Material::FromJson(old, {}, "old.mat");
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(capture.messages.empty());
    EXPECT_FLOAT_EQ(material->GetOcclusionStrength(), 1.0f);
    ExpectColor(material->GetEmissive(), 0.0f, 0.0f, 0.0f, 1.0f);
    // Saving it adds the new key and reads back the same
    const json saved = material->ToJson();
    EXPECT_FLOAT_EQ(saved.at("occlusionStrength").get<float>(), 1.0f);
    EXPECT_EQ(Material::FromJson(saved)->ToJson(), saved);
}

TEST(MaterialJsonTest, OcclusionStrengthIsReadWrittenAndRangeChecked)
{
    WarningCapture capture;
    EXPECT_FLOAT_EQ(Material::FromJson(json{{"occlusionStrength", 0.4}})->GetOcclusionStrength(), 0.4f);
    EXPECT_TRUE(capture.messages.empty());

    WarningCapture bad;
    EXPECT_FLOAT_EQ(Material::FromJson(json{{"occlusionStrength", "lots"}})->GetOcclusionStrength(), 1.0f);
    EXPECT_FLOAT_EQ(Material::FromJson(json{{"occlusionStrength", 2.0}})->GetOcclusionStrength(), 1.0f);
    EXPECT_EQ(bad.messages.size(), 2u);

    const auto material = Material::Create();
    material->SetOcclusionStrength(0.6f);
    material->SetEmissive(Common::Color{2.0f, 0.5f, 0.0f, 1.0f}); // brighter than the screen shows
    const auto loaded = Material::FromJson(material->ToJson());
    EXPECT_FLOAT_EQ(loaded->GetOcclusionStrength(), 0.6f);
    ExpectColor(loaded->GetEmissive(), 2.0f, 0.5f, 0.0f, 1.0f);
}

TEST(MaterialJsonTest, ToJsonReadsBackAsTheSameMaterial)
{
    const auto material = Material::Create(ShadingModel::Unlit);
    material->SetBaseColor(Common::Color{0.5f, 0.25f, 1.0f, 0.75f});
    material->SetAlphaMode(AlphaMode::Blend);
    material->SetAlphaCutoff(0.2f);
    material->SetDoubleSided(true);
    material->SetSmoothness(0.1f);
    material->SetMetallic(0.3f);

    const json saved = material->ToJson();
    EXPECT_EQ(saved.at("shading"), "unlit");
    EXPECT_EQ(saved.at("alphaMode"), "blend");
    EXPECT_TRUE(saved.at("baseColorTexture").is_null());

    WarningCapture capture;
    const auto loaded = Material::FromJson(saved);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(capture.messages.empty());
    EXPECT_EQ(loaded->ToJson(), saved);
}

// ============================================================================
// .mat files as project assets
// ============================================================================

class MaterialAssetTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static void WriteText(const std::string &relative, const std::string &text)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    static void WriteBytes(const std::string &relative, const std::vector<std::uint8_t> &bytes)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void SetUp() override
    {
        // No explicit Material::RegisterLoader(): ResourceLoader::Initialize registers it
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_material_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets");

        WriteBytes("textures/checker.bmp", TextureTestSupport::MakeBmp(2, 1, {{255, 0, 0}, {0, 255, 0}}));
        WriteText("materials/crate.mat", R"({
            "shading": "lit",
            "baseColor": {"r": 1, "g": 0.5, "b": 0.5, "a": 1},
            "baseColorTexture": "../textures/checker.bmp",
            "alphaMode": "mask",
            "alphaCutoff": 0.25
        })");
        WriteText("materials/byResPath.mat", R"({"baseColorTexture": "res://textures/checker.bmp"})");
        WriteText("materials/Upper.MAT", R"({"shading": "unlit"})");
        WriteText("materials/missingTexture.mat", R"({"baseColorTexture": "res://textures/nope.png", "doubleSided": true})");
        WriteText("materials/broken.mat", "{ not json");

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "MaterialAssetTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }
};

TEST_F(MaterialAssetTest, MatFilesAreScannedAsMaterialsInAnyCase)
{
    auto &loader = IO::ResourceLoader::Instance();
    for (const std::string relative : {"materials/crate.mat", "materials/Upper.MAT"})
    {
        const IO::ResourcePath path("res://" + relative);
        const IO::AssetMetadata *meta = loader.GetMetadata(path);
        ASSERT_NE(meta, nullptr) << relative;
        EXPECT_EQ(meta->resourceType, "Material") << relative;
        EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(path)) << relative;
    }
    const auto upper = IO::Resources::Instance().Load<Material>("res://materials/Upper.MAT");
    ASSERT_NE(upper, nullptr);
    EXPECT_EQ(upper->GetShading(), ShadingModel::Unlit);
}

TEST_F(MaterialAssetTest, AMatFileLoadsWithItsTexture)
{
    const IO::ResourcePath path("res://materials/crate.mat");
    const auto material = IO::Resources::Instance().Load<Material>(std::string(path.ToString()));
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->GetUUID(), IO::ResourceUUID::FromPath(path)) << "its stable .meta UUID";
    EXPECT_EQ(material->GetResourcePath(), path);
    EXPECT_FALSE(material->GetSourcePath().empty());
    EXPECT_EQ(material->GetShading(), ShadingModel::Lit);
    ExpectColor(material->GetBaseColor(), 1.0f, 0.5f, 0.5f, 1.0f);
    EXPECT_EQ(material->GetAlphaMode(), AlphaMode::Mask);
    EXPECT_FLOAT_EQ(material->GetAlphaCutoff(), 0.25f);

    // The texture path is relative to the .mat file; it loads as the project asset
    ASSERT_NE(material->GetBaseColorTexture(), nullptr);
    EXPECT_EQ(material->GetBaseColorTexture(),
              IO::ResourceLoader::Instance().GetCached<Texture>(IO::ResourcePath("res://textures/checker.bmp")));
    EXPECT_EQ(material->GetBaseColorTexture()->GetWidth(), 2u);

    // A res:// path finds the same texture, and ToJson writes it as its res:// path
    const auto byResPath = IO::Resources::Instance().Load<Material>("res://materials/byResPath.mat");
    ASSERT_NE(byResPath, nullptr);
    EXPECT_EQ(byResPath->GetBaseColorTexture(), material->GetBaseColorTexture());
    EXPECT_EQ(byResPath->ToJson().at("baseColorTexture"), "res://textures/checker.bmp");

    // Loaded again by its UUID (as a scene reference is) after the cache is cleared
    const Math::UUID uuid = material->GetUUID();
    IO::ResourceLoader::Instance().ClearCache();
    const auto again = IO::Resources::Instance().LoadByUUID<Material>(uuid);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->GetAlphaMode(), AlphaMode::Mask);
}

TEST_F(MaterialAssetTest, AMissingTextureWarnsAndABrokenFileFailsToLoad)
{
    WarningCapture capture;
    const auto missing = IO::Resources::Instance().Load<Material>("res://materials/missingTexture.mat");
    ASSERT_NE(missing, nullptr) << "the rest of the file still loads";
    EXPECT_EQ(missing->GetBaseColorTexture(), nullptr);
    EXPECT_TRUE(missing->IsDoubleSided());
    EXPECT_TRUE(capture.Mentions("nope.png"));

    EXPECT_EQ(IO::Resources::Instance().Load<Material>("res://materials/broken.mat"), nullptr);
    EXPECT_TRUE(capture.Mentions("not valid JSON"));
}
