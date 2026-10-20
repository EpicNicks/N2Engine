#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"

#include "common/CrossBackendDiff.hpp"
#include "VulkanTestFixture.hpp"

// The editor's viewport is lit by the lights of the scene it shows (#6, E4): tests/engine/rendering/
// EditModeLightingGoldenTests.cpp on the Vulkan backend (#43, stage C), with the same scenes, camera, size and
// expectations. A scene opened for editing never attaches its components, so the light list that rendering used to read is
// empty and every scene got the default light. Drawn as Application::Render draws the scene pass, on a Vulkan device (Mesa
// lavapipe in CI): lit by what CollectLighting finds, and compared by brightness only. Each frame is also drawn on a headless
// software renderer and compared with the lit criterion (mean error <= 2/255, <= 1% of the pixels over 8/255, a 1-pixel
// coverage-edge border).

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Renderer::Software::SoftwareRenderer;
using VulkanTesting::VulkanTest;

namespace
{
    constexpr int Size = 64;

    struct Frame
    {
        std::vector<std::uint8_t> rgba;

        /// The sum of a pixel's red, green and blue
        [[nodiscard]] int Brightness(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return rgba[i] + rgba[i + 1] + rgba[i + 2];
        }

        [[nodiscard]] VulkanTesting::Diff::Image AsImage() const { return {rgba, Size, Size}; }
    };

    /// A lit cube, and whatever `build` adds, drawn as Application::Render draws a scene (the lighting from the scene's
    /// CollectLighting) on `renderer` (already initialized at Size x Size)
    /// `flatMask`: the cube drawn flat white with the unlit material (the coverage mask of the lit comparison)
    Frame RenderOn(Renderer::Common::IRenderer& renderer, const bool editMode, const std::function<void(Scene&)>& build,
                   const bool flatMask = false)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
        {
            Camera camera;
            camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
            camera.SetPosition(Math::Vector3(2.5f, 0.0f, 2.5f));
            camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));

            auto scene = Scene::Create("EditModeLightingGolden");
            scene->SetEditMode(editMode);
            auto cube = GameObject::Create("Cube");
            auto* cubeRenderer = cube->AddComponent<MeshRenderer>();
            cubeRenderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Cube));
            if (flatMask)
            {
                auto flat = Rendering::Material::Create(Rendering::ShadingModel::Unlit);
                flat->SetBaseColor(Common::Color::White);
                cubeRenderer->SetMaterial(0, flat);
            }
            scene->AddRootGameObject(cube);
            build(*scene);

            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset(); // the scene goes before the renderer does
        }
        return frame;
    }

    /// A light shining down -Z, at the face of the cube that looks toward +Z (the camera sees it left of centre)
    void AddLightOnTheFront(Scene& scene)
    {
        auto lamp = GameObject::Create("Lamp");
        auto* light = lamp->AddComponent<Rendering::Light>();
        light->type = Rendering::LightType::Directional;
        light->direction = Math::Vector3(0.0f, 0.0f, -1.0f);
        light->intensity = 1.0f;
        scene.AddRootGameObject(lamp);
    }

    constexpr int FrontX = 23;
    constexpr int Row = Size / 2;

    class VulkanEditModeLightingGoldenTest : public VulkanTest
    {
    protected:
        void SetUp() override
        {
            VulkanTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            N2_REQUIRE_BUILT_IN_SHADERS();
            ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));
        }

        /// The brightness of the pixel at (x, y) (counted up from the bottom row) of the scene on Vulkan, with the lit
        /// cross-backend comparison against a headless software renderer
        int Render(const bool editMode, const std::function<void(Scene&)>& build, const int x, const int y,
                   const std::string& what)
        {
            const Frame vulkan = RenderOn(R(), editMode, build);
            SoftwareRenderer software;
            EXPECT_TRUE(software.Initialize(nullptr, Size, Size));
            const Frame reference = RenderOn(software, editMode, build);
            software.Shutdown();
            // The coverage masks: the cube drawn flat unlit on each backend
            const Frame vulkanMask = RenderOn(R(), editMode, build, true);
            SoftwareRenderer softwareForMask;
            EXPECT_TRUE(softwareForMask.Initialize(nullptr, Size, Size));
            const Frame referenceMask = RenderOn(softwareForMask, editMode, build, true);
            softwareForMask.Shutdown();
            const VulkanTesting::Diff::Image maskA = vulkanMask.AsImage();
            const VulkanTesting::Diff::Image maskB = referenceMask.AsImage();
            VulkanTesting::Diff::Options options = VulkanTesting::Diff::Lit();
            options.coverageA = &maskA;
            options.coverageB = &maskB;
            EXPECT_TRUE(VulkanTesting::Diff::Agree(vulkan.AsImage(), reference.AsImage(), options)) << what;
            const std::string described = VulkanTesting::Diff::Compare(vulkan.AsImage(), reference.AsImage(), options).Describe();
            std::cout << "[ cross-diff ] edit-mode lighting, " << what << ": " << described << std::endl;
            VulkanTesting::Diff::RecordedLines().push_back("edit-mode lighting, " + what + ": " + described);
            return vulkan.Brightness(x, y);
        }
    };
}

TEST_F(VulkanEditModeLightingGoldenTest, TheScenesLightLightsTheViewportOnlyInEditMode)
{
    // No light in the scene: the default light, whichever the mode (it comes from the front-lit side)
    const int withoutLight = Render(true, [](Scene&) {}, FrontX, Row, "no light, edit mode");
    EXPECT_EQ(Render(false, [](Scene&) {}, FrontX, Row, "no light, play mode"), withoutLight);

    // The scene's light, on a scene opened for editing: the face it shines on is brighter than under the default light
    const int editLit = Render(true, AddLightOnTheFront, FrontX, Row, "a light, edit mode");
    EXPECT_GT(editLit, withoutLight + 100) << "the scene's own light lights the viewport";

    // The same scene outside edit mode: the light never attached, so it doesn't light anything (and, as before this,
    // the default light is used instead)
    EXPECT_EQ(Render(false, AddLightOnTheFront, FrontX, Row, "a light, play mode"), withoutLight);
}

TEST_F(VulkanEditModeLightingGoldenTest, AnInactiveLightLightsNothing)
{
    const int withoutLight = Render(true, [](Scene&) {}, FrontX, Row, "no light");
    const int withInactiveLight = Render(true, [](Scene& scene)
    {
        AddLightOnTheFront(scene);
        scene.FindGameObject("Lamp")->SetActive(false);
    }, FrontX, Row, "an inactive light");
    EXPECT_EQ(withInactiveLight, withoutLight);
}
