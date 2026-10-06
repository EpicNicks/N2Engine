#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"

#include "MeshTestSupport.hpp"

// The built-in shapes (CubeRenderer, SphereRenderer, QuadRenderer) on the shared mesh path: every shape of a kind on
// a renderer shares one GPU mesh and (without a material) one GPU material, held as GpuCache shares under the
// renderer lifetime rules. The scenarios are the ones the per-component resources had before #3 P2 (a renderer
// recreated at the same address, a switch to another renderer, a shutdown and re-initialise, destruction after the
// renderer is gone), checked through the cache's entries and users. Plus what the shapes draw, and scenes saved
// before P2. No window or GPU is involved.

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::GpuCache;
using Rendering::Material;
using Rendering::Mesh;
using Renderer::Common::IndexRange;
using Renderer::Common::RenderState;
using MeshTestSupport::Counts;
using MeshTestSupport::RecordingMeshRenderer;
using json = nlohmann::json;

namespace
{
    // A polygon renderer on its own object; the object is returned so the component lives as long as it
    template <typename T>
    struct PolygonObject
    {
        GameObject::Ptr gameObject;
        T *renderer = nullptr;
    };

    template <typename T>
    PolygonObject<T> MakePolygon(const std::string &name)
    {
        PolygonObject<T> result;
        result.gameObject = GameObject::Create(name);
        result.renderer = result.gameObject->AddComponent<T>();
        return result;
    }

    std::size_t MeshUsers(const Renderer::Common::IRenderer &renderer, const BuiltinMesh which)
    {
        return GpuCache::GetUserCount(renderer, Mesh::GetBuiltin(which).get(), GpuCache::ResourceKind::Mesh);
    }

    std::size_t UnlitUsers(const Renderer::Common::IRenderer &renderer)
    {
        const auto unlit = Material::GetDefaultUnlit();
        return GpuCache::GetUserCount(renderer, unlit.get(), GpuCache::ResourceKind::Material, unlit->GetGpuVersion());
    }
}

// ============================================================================
// Lifetime (the pre-P2 scenarios, on shared resources)
// ============================================================================

TEST(PolygonRendererLifetimeTest, TheSameRendererKeepsItsResources)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    auto object = MakePolygon<Example::CubeRenderer>("Kept");

    object.renderer->Render(&renderer);
    object.renderer->Render(&renderer);

    EXPECT_EQ(renderer.GetCounts().createdMeshes, 1);
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1);
    EXPECT_EQ(renderer.GetCounts().Destroys(), 0);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 2) << "the cube's mesh and the unlit material";
    EXPECT_EQ(MeshUsers(renderer, BuiltinMesh::Cube), 1u);
    EXPECT_EQ(UnlitUsers(renderer), 1u);
    ASSERT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(renderer.draws[0].mesh, renderer.draws[1].mesh);

    object.renderer->CleanupRenderResources(&renderer);
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(PolygonRendererLifetimeTest, ShapesOfAKindShareOneGpuMeshAndMaterial)
{
    RecordingMeshRenderer renderer;
    auto red = MakePolygon<Example::CubeRenderer>("Red");
    auto blue = MakePolygon<Example::CubeRenderer>("Blue");
    auto quad = MakePolygon<Example::QuadRenderer>("Quad");
    red.renderer->SetColor(Common::Color::Red);
    blue.renderer->SetColor(Common::Color::Blue);

    red.renderer->Render(&renderer);
    blue.renderer->Render(&renderer);
    quad.renderer->Render(&renderer);

    EXPECT_EQ(renderer.GetCounts().createdMeshes, 2) << "one cube, one quad";
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1) << "one unlit material, coloured per draw";
    EXPECT_EQ(MeshUsers(renderer, BuiltinMesh::Cube), 2u);
    EXPECT_EQ(MeshUsers(renderer, BuiltinMesh::Quad), 1u);
    EXPECT_EQ(UnlitUsers(renderer), 3u);

    // Each draw still has its own colour
    ASSERT_EQ(renderer.draws.size(), 3u);
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[0], 1.0f);
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[2], 0.0f);
    EXPECT_FLOAT_EQ(renderer.draws[1].albedo[0], 0.0f);
    EXPECT_FLOAT_EQ(renderer.draws[1].albedo[2], 1.0f);

    red.renderer->OnDestroy();
    EXPECT_EQ(renderer.GetCounts().Destroys(), 0) << "still in use by the others";
    blue.renderer->OnDestroy();
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 1) << "the last cube destroys the cube mesh";
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 0) << "the quad still uses the material";
    quad.renderer->OnDestroy();
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
}

TEST(PolygonRendererLifetimeTest, ARendererRecreatedAtTheSameAddressGetsNewResources)
{
    // Window recreates its renderer, and the new one can land where the old one was. The component
    // isn't told, so it must not take the new renderer for the old one.
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    auto object = MakePolygon<Example::CubeRenderer>("Recreated");
    object.renderer->Render(&*renderer);
    ASSERT_EQ(counts.createdMeshes, 1);

    renderer.reset();         // frees everything it made
    renderer.emplace(counts); // same storage, so the same address
    EXPECT_EQ(MeshUsers(*renderer, BuiltinMesh::Cube), 0u) << "the old entry isn't the new renderer's";

    object.renderer->Render(&*renderer);
    EXPECT_EQ(counts.createdMeshes, 2); // a new mesh and material, made on the new renderer
    EXPECT_EQ(counts.createdMaterials, 2);
    EXPECT_EQ(counts.Destroys(), 0); // nothing of the old renderer's is destroyed here
    EXPECT_EQ(MeshUsers(*renderer, BuiltinMesh::Cube), 1u);
    ASSERT_EQ(renderer->draws.size(), 1u);
    EXPECT_TRUE(renderer->OwnsMesh(renderer->draws[0].mesh));
    EXPECT_TRUE(renderer->OwnsMaterial(renderer->draws[0].material));

    object.renderer->CleanupRenderResources(&*renderer);
    EXPECT_TRUE(renderer->meshes.empty());
    EXPECT_TRUE(renderer->materials.empty());
}

TEST(PolygonRendererLifetimeTest, ANewRendererReleasesWhatTheOldOneHeld)
{
    RecordingMeshRenderer first;
    RecordingMeshRenderer second;
    auto object = MakePolygon<Example::QuadRenderer>("Switch");

    object.renderer->Render(&first);
    object.renderer->Render(&second);

    EXPECT_TRUE(first.meshes.empty());
    EXPECT_TRUE(first.materials.empty());
    EXPECT_EQ(first.GetCounts().destroyedMeshes, 1);
    EXPECT_EQ(first.GetCounts().destroyedMaterials, 1);
    EXPECT_EQ(MeshUsers(first, BuiltinMesh::Quad), 0u);
    EXPECT_EQ(second.GetCounts().createdMeshes, 1);
    EXPECT_EQ(second.GetCounts().Destroys(), 0);
    EXPECT_EQ(MeshUsers(second, BuiltinMesh::Quad), 1u);
    ASSERT_EQ(second.draws.size(), 1u);
    EXPECT_TRUE(second.OwnsMesh(second.draws[0].mesh));

    // Cleanup on a renderer it holds nothing on does nothing
    object.renderer->CleanupRenderResources(&first);
    EXPECT_EQ(second.GetCounts().Destroys(), 0);
    EXPECT_EQ(first.GetCounts().destroyedMeshes, 1);

    object.renderer->CleanupRenderResources(&second);
    EXPECT_TRUE(second.meshes.empty());
    EXPECT_TRUE(second.materials.empty());
}

TEST(PolygonRendererLifetimeTest, ARendererShutDownAndInitialisedAgainCountsAsNew)
{
    RecordingMeshRenderer renderer;
    auto object = MakePolygon<Example::CubeRenderer>("Reinitialised");
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.GetCounts().createdMeshes, 1);

    // Shutdown frees everything the renderer made; the same object is then initialised again
    renderer.Shutdown();
    renderer.Initialize(nullptr, 1, 1);
    EXPECT_EQ(MeshUsers(renderer, BuiltinMesh::Cube), 0u);

    object.renderer->Render(&renderer);
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 2); // new resources, not the freed ones
    EXPECT_EQ(renderer.GetCounts().Destroys(), 0);    // the old shares were forgotten, not destroyed
    EXPECT_EQ(MeshUsers(renderer, BuiltinMesh::Cube), 1u);
    object.renderer->OnDestroy();
}

TEST(PolygonRendererLifetimeTest, DestroyingAfterTheRendererIsGoneCallsNothing)
{
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    auto object = MakePolygon<Example::CubeRenderer>("Orphaned");
    object.renderer->Render(&*renderer);
    const std::size_t entries = GpuCache::GetEntryCount();

    renderer.reset();
    renderer.emplace(counts); // a new renderer where the old one was

    // The resources died with the old renderer, so they are forgotten, not destroyed on the new one
    object.renderer->OnDestroy();
    EXPECT_EQ(counts.Destroys(), 0);
    EXPECT_EQ(GpuCache::GetEntryCount(), entries - 2) << "the old entries are gone";

    // Drawing again makes new ones
    object.renderer->Render(&*renderer);
    EXPECT_EQ(counts.createdMeshes, 2);
    EXPECT_EQ(counts.Destroys(), 0);
    object.renderer->CleanupRenderResources(&*renderer);
    EXPECT_TRUE(renderer->meshes.empty());
}

// ============================================================================
// What the shapes draw
// ============================================================================

TEST(PolygonRendererDrawTest, ShapesDrawTheirBuiltinMeshUnlitInTheirColourScaledByTheirSize)
{
    RecordingMeshRenderer renderer;
    auto object = MakePolygon<Example::CubeRenderer>("Shape");
    object.renderer->SetColor(Common::Color{0.25f, 0.5f, 0.75f, 0.5f});
    object.renderer->SetSize(Math::Vector3(2.0f, 3.0f, 4.0f));
    object.gameObject->GetPositionable()->SetPosition(Math::Vector3(1.0f, 0.0f, -5.0f));
    EXPECT_EQ(object.renderer->GetMesh(), Mesh::GetBuiltin(BuiltinMesh::Cube));
    EXPECT_EQ(object.renderer->GetRenderQueue().queue, RenderQueue::Opaque);

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    const auto &draw = renderer.draws[0];
    EXPECT_EQ(draw.range, (IndexRange{0, 36}));
    EXPECT_EQ(RecordingMeshRenderer::AsSW(draw.material)->GetShader(), &renderer.unlitShader) << "unlit";
    EXPECT_FLOAT_EQ(draw.albedo[0], 0.25f);
    EXPECT_FLOAT_EQ(draw.albedo[1], 0.5f);
    EXPECT_FLOAT_EQ(draw.albedo[2], 0.75f);
    EXPECT_FLOAT_EQ(draw.albedo[3], 0.5f);
    // A translucent colour no longer blends: the Opaque queue doesn't (#3 P2)
    EXPECT_EQ(draw.state, RenderState::Opaque());
    EXPECT_FALSE(draw.state.blend);
    // Row-major: the size scales the axes, the position translates
    EXPECT_FLOAT_EQ(draw.model[0], 2.0f);
    EXPECT_FLOAT_EQ(draw.model[5], 3.0f);
    EXPECT_FLOAT_EQ(draw.model[10], 4.0f);
    EXPECT_FLOAT_EQ(draw.model[3], 1.0f);
    EXPECT_FLOAT_EQ(draw.model[11], -5.0f);
    object.renderer->OnDestroy();
}

TEST(PolygonRendererDrawTest, AMaterialReplacesTheUnlitDefaultTintedByTheColour)
{
    RecordingMeshRenderer renderer;
    auto object = MakePolygon<Example::QuadRenderer>("Glass");
    const auto glass = Material::Create(Rendering::ShadingModel::Lit);
    glass->SetAlphaMode(Rendering::AlphaMode::Blend);
    glass->SetBaseColor(Common::Color{1.0f, 1.0f, 1.0f, 0.5f});
    object.renderer->SetMaterial(glass);
    object.renderer->SetColor(Common::Color::Green);
    EXPECT_EQ(object.renderer->GetRenderQueue().queue, RenderQueue::Transparent);

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(RecordingMeshRenderer::AsSW(renderer.draws[0].material)->GetShader(), &renderer.litShader);
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[0], 0.0f);
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[1], 1.0f);
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[3], 0.5f);
    EXPECT_EQ(renderer.draws[0].state, RenderState::Transparent()) << "Blend: the Transparent queue's state";
    object.renderer->OnDestroy();
}

TEST(PolygonRendererDrawTest, SpheresOfTheDefaultSubdivisionShareTheBuiltinAndOthersMakeTheirOwn)
{
    auto object = MakePolygon<Example::SphereRenderer>("Sphere");
    EXPECT_EQ(object.renderer->GetMesh(), Mesh::GetBuiltin(BuiltinMesh::Sphere));

    object.renderer->SetSubdivision(8, 12);
    const auto custom = object.renderer->GetMesh();
    ASSERT_NE(custom, nullptr);
    EXPECT_FALSE(custom->IsBuiltin());
    EXPECT_EQ(custom->GetVertices().size(), 9u * 13u);
    EXPECT_EQ(object.renderer->GetMesh(), custom) << "made once per subdivision";

    object.renderer->SetSubdivision(16, 32);
    EXPECT_EQ(object.renderer->GetMesh(), Mesh::GetBuiltin(BuiltinMesh::Sphere));
}

// ============================================================================
// Scenes saved before #3 P2
// ============================================================================

TEST(PolygonRendererCompatibilityTest, APreP2SceneLoadsUnchanged)
{
    // A GameObject exactly as master saved one before P2: a CubeRenderer and a SphereRenderer with only _color and
    // _size (and the sphere's subdivision), no _material
    const json saved = json::parse(R"({
        "uuid": "11111111-2222-4333-8444-555555555555",
        "name": "OldShapes",
        "tag": "Untagged",
        "layer": 0,
        "isActive": true,
        "positionable": {
            "localPosition": {"x": 1.0, "y": 2.0, "z": 3.0},
            "localRotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0},
            "localScale": {"x": 1.0, "y": 1.0, "z": 1.0}
        },
        "components": [
            {"type": "CubeRenderer", "data": {
                "uuid": "11111111-2222-4333-8444-666666666666",
                "isActive": true,
                "_color": {"r": 0.0, "g": 1.0, "b": 1.0, "a": 1.0},
                "_size": {"x": 1.0, "y": 2.0, "z": 3.0}
            }},
            {"type": "SphereRenderer", "data": {
                "uuid": "11111111-2222-4333-8444-777777777777",
                "isActive": false,
                "_color": {"r": 1.0, "g": 0.0, "b": 1.0, "a": 1.0},
                "_size": {"x": 2.5, "y": 2.5, "z": 2.5},
                "_latitudeSegments": 8,
                "_longitudeSegments": 12
            }}
        ],
        "children": []
    })");

    MeshTestSupport::WarningCapture capture;
    const auto go = GameObject::Deserialize(saved);
    ASSERT_NE(go, nullptr);
    EXPECT_TRUE(capture.messages.empty()) << capture.messages.front();

    const auto *cube = go->GetComponent<Example::CubeRenderer>();
    ASSERT_NE(cube, nullptr);
    EXPECT_FLOAT_EQ(cube->GetColor().r, 0.0f);
    EXPECT_FLOAT_EQ(cube->GetColor().g, 1.0f);
    EXPECT_FLOAT_EQ(cube->GetColor().b, 1.0f);
    EXPECT_FLOAT_EQ(cube->GetSize().y, 2.0f);
    EXPECT_FLOAT_EQ(cube->GetSize().z, 3.0f);
    EXPECT_EQ(cube->GetMaterial(), nullptr) << "no material: unlit in its colour, as before";
    EXPECT_TRUE(cube->IsActive());

    auto *sphere = go->GetComponent<Example::SphereRenderer>();
    ASSERT_NE(sphere, nullptr);
    EXPECT_FLOAT_EQ(sphere->GetRadius(), 2.5f);
    EXPECT_EQ(sphere->GetLatitudeSegments(), 8u);
    EXPECT_EQ(sphere->GetLongitudeSegments(), 12u);
    EXPECT_FALSE(sphere->IsActive());
    EXPECT_EQ(sphere->GetMaterial(), nullptr);

    // It draws as before: the cube's geometry, unlit, in its colour
    RecordingMeshRenderer renderer;
    go->GetComponent<Example::CubeRenderer>()->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].range, (IndexRange{0, 36}));
    EXPECT_FLOAT_EQ(renderer.draws[0].albedo[1], 1.0f);
    EXPECT_FLOAT_EQ(renderer.draws[0].model[3], 1.0f) << "at its saved position";
    go->GetComponent<Example::CubeRenderer>()->OnDestroy();

    // Saved again, it gains an empty _material and keeps everything else
    const json resaved = cube->Serialize();
    EXPECT_TRUE(resaved.at("_material").is_null());
    EXPECT_EQ(resaved.at("_color"), saved["components"][0]["data"]["_color"]);
    EXPECT_EQ(resaved.at("_size"), saved["components"][0]["data"]["_size"]);
}
