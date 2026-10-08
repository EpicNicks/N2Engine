#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/RenderSettings.hpp"

// The colour space setting: parsing, the project settings "rendering" block, what CollectLighting hands the
// renderer, and a scene drawn through the whole path (default gamma unchanged; linear differs and brightens midtones).

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::RenderSettings;
using Renderer::Common::ColorSpace;
using Renderer::Software::SoftwareRenderer;
using json = nlohmann::json;

namespace
{
    constexpr int Size = 64;

    /// Restores the default colour space around a test: the setting is process-wide
    class ColorSpaceGuard
    {
    public:
        ColorSpaceGuard() { Reset(); }
        ~ColorSpaceGuard() { Reset(); }

    private:
        static void Reset()
        {
            RenderSettings::SetColorSpace(ColorSpace::Gamma);
            RenderSettings::SetForceShaderEncode(false);
        }
    };

    /// A lit mid-grey cube drawn as Application::Render draws the scene pass, with the lighting CollectLighting gives
    std::vector<std::uint8_t> RenderCube()
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(Size) * Size * 4);
        {
            Camera camera;
            camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
            camera.SetPosition(Math::Vector3(2.5f, 1.5f, 2.5f));
            camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));

            auto scene = Scene::Create("RenderSettingsGolden");
            auto cube = GameObject::Create("Cube");
            auto *meshRenderer = cube->AddComponent<MeshRenderer>();
            meshRenderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Cube));
            auto grey = Material::Create();
            grey->SetBaseColor(Common::Color{0.5f, 0.5f, 0.5f, 1.0f});
            meshRenderer->SetMaterial(0, grey);
            scene->AddRootGameObject(cube);

            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(rgba.data(), Size, Size);
            scene.reset(); // the scene goes before the renderer shuts down
        }
        renderer.Shutdown();
        return rgba;
    }

    long long Sum(const std::vector<std::uint8_t> &rgba)
    {
        long long sum = 0;
        for (const std::uint8_t value : rgba)
        {
            sum += value;
        }
        return sum;
    }
}

TEST(RenderSettingsTest, TheColourSpaceIsGammaByDefault)
{
    const ColorSpaceGuard guard;
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, NamesParseCaseInsensitivelyAndRoundTrip)
{
    EXPECT_EQ(RenderSettings::ParseColorSpace("gamma"), ColorSpace::Gamma);
    EXPECT_EQ(RenderSettings::ParseColorSpace("Linear"), ColorSpace::Linear);
    EXPECT_EQ(RenderSettings::ParseColorSpace("LINEAR"), ColorSpace::Linear);
    EXPECT_FALSE(RenderSettings::ParseColorSpace("srgb").has_value());
    EXPECT_FALSE(RenderSettings::ParseColorSpace("").has_value());
    EXPECT_EQ(RenderSettings::ColorSpaceName(ColorSpace::Gamma), "gamma");
    EXPECT_EQ(RenderSettings::ColorSpaceName(ColorSpace::Linear), "linear");
}

TEST(RenderSettingsTest, TheProjectSettingsRenderingBlockSetsTheColourSpace)
{
    const ColorSpaceGuard guard;
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"colorSpace", "linear"}}}}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"colorSpace", "Gamma"}}}}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);
}

TEST(RenderSettingsTest, SettingsWithoutARenderingBlockChangeNothing)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json::object()).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", nullptr}}).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"physics", json::object()}}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, ABlockWithoutAColourSpaceMeansTheDefault)
{
    const ColorSpaceGuard guard;
    for (const json &block : {json::object(), json{{"colorSpace", nullptr}}, json{{"somethingElse", 3}}})
    {
        RenderSettings::SetColorSpace(ColorSpace::Linear);
        EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", block}}).empty()) << block.dump();
        EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma) << block.dump();
    }
}

TEST(RenderSettingsTest, APatchThatRemovesTheBlockResetsTheColourSpace)
{
    const ColorSpaceGuard guard;
    const std::set<std::string> touched{"rendering"};
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json::object(), touched).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);

    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", nullptr}}, touched).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);

    // A patch that touches other blocks only leaves it alone
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json::object(), std::set<std::string>{"physics"}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, TheSnapshotPutsTheColourSpaceBackAfterARefusedPatch)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    const ProjectSettingsSnapshot snapshot = ProjectSettingsSnapshot::Capture();
    EXPECT_EQ(snapshot.colorSpace, ColorSpace::Linear);

    // One block applies, another is refused; the caller undoes the whole patch
    const auto problems = ApplyProjectSettings(json{{"rendering", {{"colorSpace", "gamma"}}}, {"physics", 5}});
    EXPECT_EQ(problems.size(), 1u);
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);
    snapshot.Restore();
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, ABadValueIsReportedAndChangesNothing)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    const auto unknown = ApplyProjectSettings(json{{"rendering", {{"colorSpace", "srgb"}}}});
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_NE(unknown[0].find("rendering.colorSpace"), std::string::npos);
    EXPECT_EQ(ApplyProjectSettings(json{{"rendering", {{"colorSpace", 1}}}}).size(), 1u);
    EXPECT_EQ(ApplyProjectSettings(json{{"rendering", 5}}).size(), 1u);
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, OnlyRenderingLimitsWhichBlocksApply)
{
    const ColorSpaceGuard guard;
    const json settings = {{"rendering", {{"colorSpace", "linear"}}}};
    EXPECT_TRUE(ApplyProjectSettings(settings, std::set<std::string>{"physics"}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Gamma);
    EXPECT_TRUE(ApplyProjectSettings(settings, std::set<std::string>{"rendering"}).empty());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);
}

TEST(RenderSettingsTest, ForceShaderEncodeIsOffByDefaultAndSetByTheProjectSetting)
{
    const ColorSpaceGuard guard;
    EXPECT_FALSE(RenderSettings::GetForceShaderEncode());
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"colorSpace", "linear"}, {"forceShaderEncode", true}}}}).empty());
    EXPECT_TRUE(RenderSettings::GetForceShaderEncode());
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear);

    // Removing the key, or the whole block with a patch, turns it off again
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"colorSpace", "linear"}}}}).empty());
    EXPECT_FALSE(RenderSettings::GetForceShaderEncode());
    RenderSettings::SetForceShaderEncode(true);
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"forceShaderEncode", nullptr}}}}).empty());
    EXPECT_FALSE(RenderSettings::GetForceShaderEncode());
    RenderSettings::SetForceShaderEncode(true);
    EXPECT_TRUE(ApplyProjectSettings(json::object(), std::set<std::string>{"rendering"}).empty());
    EXPECT_FALSE(RenderSettings::GetForceShaderEncode());
}

TEST(RenderSettingsTest, ABadForceShaderEncodeIsRefusedAndChangesNothingElse)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetForceShaderEncode(true);
    const auto problems = ApplyProjectSettings(json{{"rendering", {{"forceShaderEncode", "yes"}, {"colorSpace", "linear"}}}});
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("rendering.forceShaderEncode"), std::string::npos);
    EXPECT_TRUE(RenderSettings::GetForceShaderEncode()) << "the refused value changed nothing";
    EXPECT_EQ(RenderSettings::GetColorSpace(), ColorSpace::Linear) << "the valid key still applied";
}

TEST(RenderSettingsTest, TheSnapshotAlsoRestoresForceShaderEncode)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetForceShaderEncode(true);
    const ProjectSettingsSnapshot snapshot = ProjectSettingsSnapshot::Capture();
    EXPECT_TRUE(snapshot.forceShaderEncode);
    RenderSettings::SetForceShaderEncode(false);
    snapshot.Restore();
    EXPECT_TRUE(RenderSettings::GetForceShaderEncode());
}

TEST(RenderSettingsTest, CollectLightingCarriesTheColourSpace)
{
    const ColorSpaceGuard guard;
    auto scene = Scene::Create("RenderSettingsLighting");
    EXPECT_EQ(scene->CollectLighting().colorSpace, ColorSpace::Gamma);
    EXPECT_FALSE(scene->CollectLighting().forceShaderEncode);
    RenderSettings::SetForceShaderEncode(true);
    EXPECT_TRUE(scene->CollectLighting().forceShaderEncode);
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_EQ(scene->CollectLighting().colorSpace, ColorSpace::Linear);
    EXPECT_EQ(scene->CollectLighting(Scene::LightingSource::Hierarchy).colorSpace, ColorSpace::Linear);
}

TEST(RenderSettingsTest, ASceneDrawsLinearDifferentlyFromGammaAndTheDefaultIsGamma)
{
    const ColorSpaceGuard guard;
    const std::vector<std::uint8_t> byDefault = RenderCube();
    RenderSettings::SetColorSpace(ColorSpace::Gamma);
    const std::vector<std::uint8_t> gamma = RenderCube();
    EXPECT_EQ(byDefault, gamma) << "gamma is what the engine always drew";

    RenderSettings::SetColorSpace(ColorSpace::Linear);
    const std::vector<std::uint8_t> linear = RenderCube();
    EXPECT_NE(linear, gamma);
    RenderSettings::SetColorSpace(ColorSpace::Gamma);
    EXPECT_EQ(RenderCube(), byDefault) << "going back to gamma gives the default picture again";
    // Encoding for display lifts midtones; the black background is unchanged, so the total goes up
    EXPECT_GT(Sum(linear), Sum(gamma));
}
