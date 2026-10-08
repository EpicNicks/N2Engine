#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"

// The editor's viewport is lit by the lights of the scene it shows (#6, E4). A scene opened for editing never attaches
// its components, so the light list that rendering used to read is empty and every scene got the default light. Drawn
// here as Application::Render draws the scene pass, on a headless software renderer: lit by what CollectLighting
// finds, and compared by brightness only.

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 64;

    /// The sum of a pixel's red, green and blue
    int Brightness(const std::vector<std::uint8_t> &rgba, const int x, const int y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
        return rgba[i] + rgba[i + 1] + rgba[i + 2];
    }

    /// A lit cube, and whatever `build` adds, drawn as Application::Render draws a scene (the lighting from the
    /// scene's CollectLighting); the brightness of the pixel at (x, y) counted up from the bottom row
    int Render(const bool editMode, const std::function<void(Scene &)> &build, const int x, const int y)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(Size) * Size * 4);
        {
            Camera camera;
            camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
            camera.SetPosition(Math::Vector3(2.5f, 0.0f, 2.5f));
            camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));

            auto scene = Scene::Create("EditModeLightingGolden");
            scene->SetEditMode(editMode);
            auto cube = GameObject::Create("Cube");
            cube->AddComponent<MeshRenderer>()->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Cube));
            scene->AddRootGameObject(cube);
            build(*scene);

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
        return Brightness(rgba, x, y);
    }

    /// A light shining down -Z, at the face of the cube that looks toward +Z (the camera sees it left of centre)
    void AddLightOnTheFront(Scene &scene)
    {
        auto lamp = GameObject::Create("Lamp");
        auto *light = lamp->AddComponent<Rendering::Light>();
        light->type = Rendering::LightType::Directional;
        light->direction = Math::Vector3(0.0f, 0.0f, -1.0f);
        light->intensity = 1.0f;
        scene.AddRootGameObject(lamp);
    }

    constexpr int FrontX = 23;
    constexpr int Row = Size / 2;
}

TEST(EditModeLightingGoldenTest, TheScenesLightLightsTheViewportOnlyInEditMode)
{
    // No light in the scene: the default light, whichever the mode (it comes from the front-lit side)
    const int withoutLight = Render(true, [](Scene &) {}, FrontX, Row);
    EXPECT_EQ(Render(false, [](Scene &) {}, FrontX, Row), withoutLight);

    // The scene's light, on a scene opened for editing: the face it shines on is brighter than under the default light
    const int editLit = Render(true, AddLightOnTheFront, FrontX, Row);
    EXPECT_GT(editLit, withoutLight + 100) << "the scene's own light lights the viewport";

    // The same scene outside edit mode: the light never attached, so it doesn't light anything (and, as before this,
    // the default light is used instead)
    EXPECT_EQ(Render(false, AddLightOnTheFront, FrontX, Row), withoutLight);
}

TEST(EditModeLightingGoldenTest, AnInactiveLightLightsNothing)
{
    const int withoutLight = Render(true, [](Scene &) {}, FrontX, Row);
    const int withInactiveLight = Render(true, [](Scene &scene)
    {
        AddLightOnTheFront(scene);
        scene.FindGameObject("Lamp")->SetActive(false);
    }, FrontX, Row);
    EXPECT_EQ(withInactiveLight, withoutLight);
}
