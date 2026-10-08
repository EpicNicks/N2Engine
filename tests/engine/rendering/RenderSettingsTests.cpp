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
        ColorSpaceGuard() { RenderSettings::SetColorSpace(ColorSpace::Gamma); }
        ~ColorSpaceGuard() { RenderSettings::SetColorSpace(ColorSpace::Gamma); }
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

TEST(RenderSettingsTest, ASettingsObjectWithoutTheBlockOrTheKeyChangesNothing)
{
    const ColorSpaceGuard guard;
    RenderSettings::SetColorSpace(ColorSpace::Linear);
    EXPECT_TRUE(ApplyProjectSettings(json::object()).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", json::object()}}).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"colorSpace", nullptr}}}}).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", {{"somethingElse", 3}}}}).empty()) << "other keys are left alone";
    EXPECT_TRUE(ApplyProjectSettings(json{{"rendering", nullptr}}).empty());
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

TEST(RenderSettingsTest, CollectLightingCarriesTheColourSpace)
{
    const ColorSpaceGuard guard;
    auto scene = Scene::Create("RenderSettingsLighting");
    EXPECT_EQ(scene->CollectLighting().colorSpace, ColorSpace::Gamma);
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
    // Encoding for display lifts midtones; the black background is unchanged, so the total goes up
    EXPECT_GT(Sum(linear), Sum(gamma));
}
