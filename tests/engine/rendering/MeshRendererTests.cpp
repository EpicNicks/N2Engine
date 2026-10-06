#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

#include "MeshTestSupport.hpp"

// MeshRenderer: what it draws (submeshes as index ranges, materials, queues, cull state), its GPU shares in the
// GpuCache under the renderer lifetime rules, and its serialization. Drawn through a renderer that records every
// call; no window or GPU.

using namespace N2Engine;
using Rendering::AlphaMode;
using Rendering::BuiltinMesh;
using Rendering::GpuCache;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::ShadingModel;
using Rendering::Submesh;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::Vertex;
using MeshTestSupport::Counts;
using MeshTestSupport::RecordingMeshRenderer;

namespace
{
    /// Two quads in one mesh (indices 0..5 and 6..11), one submesh each
    std::shared_ptr<Mesh> TwoSubmeshMesh()
    {
        MeshData data = Mesh::MakeQuad();
        const MeshData right = Mesh::MakeQuad();
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (Vertex vertex : right.vertices)
        {
            vertex.position[0] += 2.0f;
            data.vertices.push_back(vertex);
        }
        for (const std::uint32_t index : right.indices)
        {
            data.indices.push_back(base + index);
        }
        return Mesh::Create(std::move(data), {Submesh{0, 6, {}}, Submesh{6, 6, {}}});
    }

    std::shared_ptr<Material> MaterialWith(const AlphaMode mode, const ShadingModel shading = ShadingModel::Unlit)
    {
        auto material = Material::Create(shading);
        material->SetAlphaMode(mode);
        return material;
    }

    struct MeshObject
    {
        GameObject::Ptr gameObject;
        MeshRenderer *renderer = nullptr;
    };

    MeshObject MakeMeshObject(const std::string &name, std::shared_ptr<Mesh> mesh, const Math::Vector3 &position = {})
    {
        MeshObject result;
        result.gameObject = GameObject::Create(name);
        result.renderer = result.gameObject->AddComponent<MeshRenderer>();
        result.renderer->SetMesh(std::move(mesh));
        result.gameObject->GetPositionable()->SetPosition(position);
        return result;
    }
}

// ============================================================================
// Drawing
// ============================================================================

TEST(MeshRendererTest, DrawsEachSubmeshAsAnIndexRangeWithTheDefaultLitMaterial)
{
    RecordingMeshRenderer renderer;
    auto object = MakeMeshObject("Two", TwoSubmeshMesh());
    EXPECT_EQ(object.renderer->GetMaterialCount(), 2u);

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto &draw = renderer.draws[i];
        EXPECT_TRUE(draw.hasRange);
        EXPECT_EQ(draw.range, (IndexRange{static_cast<std::uint32_t>(6 * i), 6}));
        EXPECT_EQ(draw.state, RenderState::Opaque()) << "opaque materials don't blend";
        EXPECT_FALSE(draw.state.blend);
        EXPECT_EQ(RecordingMeshRenderer::AsSW(draw.material)->GetShader(), &renderer.litShader) << "lit white";
        EXPECT_FLOAT_EQ(draw.albedo[0], 1.0f);
    }
    EXPECT_EQ(renderer.draws[0].mesh, renderer.draws[1].mesh) << "one GPU mesh";
    EXPECT_EQ(renderer.draws[0].material, renderer.draws[1].material) << "the default material, shared";
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 1);
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1);
    object.renderer->OnDestroy();
}

TEST(MeshRendererTest, RenderWithoutARendererOrAMeshDoesNothing)
{
    auto object = MakeMeshObject("Empty", nullptr);
    EXPECT_NO_THROW(object.renderer->Render(nullptr));
    RecordingMeshRenderer renderer;
    object.renderer->Render(&renderer);
    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_EQ(object.renderer->GetMaterialCount(), 0u);
    EXPECT_FALSE(object.renderer->GetBounds().has_value());
    EXPECT_FALSE(object.renderer->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_FALSE(object.renderer->DrawsInQueue(RenderQueue::Transparent));

    object.renderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Cube));
    EXPECT_NO_THROW(object.renderer->Render(nullptr));
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 0);
}

TEST(MeshRendererTest, MaterialSlotsFollowTheSubmeshesAndEmptySlotsUseTheDefault)
{
    RecordingMeshRenderer renderer;
    auto object = MakeMeshObject("Slots", TwoSubmeshMesh());
    const auto red = Material::Create(ShadingModel::Unlit);
    red->SetBaseColor(Common::Color::Red);
    object.renderer->SetMaterial(1, red);
    ASSERT_EQ(object.renderer->GetMaterials().size(), 2u) << "grown to reach slot 1";
    EXPECT_EQ(object.renderer->GetMaterial(0), nullptr);
    EXPECT_EQ(object.renderer->GetMaterial(1), red);
    EXPECT_EQ(object.renderer->GetMaterial(7), nullptr);
    EXPECT_EQ(object.renderer->GetEffectiveMaterial(0), Material::GetDefault());
    EXPECT_EQ(object.renderer->GetEffectiveMaterial(1), red);

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(RecordingMeshRenderer::AsSW(renderer.draws[0].material)->GetShader(), &renderer.litShader);
    EXPECT_EQ(RecordingMeshRenderer::AsSW(renderer.draws[1].material)->GetShader(), &renderer.unlitShader);
    EXPECT_FLOAT_EQ(renderer.draws[1].albedo[0], 1.0f);
    EXPECT_FLOAT_EQ(renderer.draws[1].albedo[1], 0.0f) << "red";

    // Slots past the last submesh are kept but unused
    object.renderer->SetMaterial(5, red);
    renderer.draws.clear();
    object.renderer->Render(&renderer);
    EXPECT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(object.renderer->GetMaterials().size(), 6u);
    object.renderer->OnDestroy();
}

TEST(MeshRendererTest, MaskMaterialsAlphaTestInTheOpaqueQueue)
{
    RecordingMeshRenderer renderer;
    auto object = MakeMeshObject("Cutout", Mesh::GetBuiltin(BuiltinMesh::Quad));
    const auto mask = MaterialWith(AlphaMode::Mask);
    mask->SetAlphaCutoff(0.3f);
    object.renderer->SetMaterial(0, mask);
    EXPECT_EQ(object.renderer->GetRenderQueue().queue, RenderQueue::Opaque);
    EXPECT_TRUE(object.renderer->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_FALSE(object.renderer->DrawsInQueue(RenderQueue::Transparent));

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_FLOAT_EQ(renderer.draws[0].alphaCutoff, 0.3f);
    EXPECT_EQ(renderer.draws[0].state, RenderState::Opaque());

    // Opaque: no cutoff
    object.renderer->SetMaterial(0, MaterialWith(AlphaMode::Opaque));
    renderer.draws.clear();
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_FLOAT_EQ(renderer.draws[0].alphaCutoff, 0.0f);
    object.renderer->OnDestroy();
}

TEST(MeshRendererTest, DoubleSidedMaterialsDrawWithoutCulling)
{
    RecordingMeshRenderer renderer;
    auto object = MakeMeshObject("TwoSided", Mesh::GetBuiltin(BuiltinMesh::Quad));
    const auto twoSided = MaterialWith(AlphaMode::Opaque);
    twoSided->SetDoubleSided(true);
    object.renderer->SetMaterial(0, twoSided);

    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].state.cull, CullMode::None);
    EXPECT_TRUE(renderer.draws[0].state.depthWrite);

    // Mirrored too: still none
    object.gameObject->GetPositionable()->SetLocalScale(Math::Vector3(-1.0f, 1.0f, 1.0f));
    renderer.draws.clear();
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].state.cull, CullMode::None);
    object.renderer->OnDestroy();
}

TEST(MeshRendererTest, AMirroredObjectCullsFrontFaces)
{
    RecordingMeshRenderer renderer;
    auto object = MakeMeshObject("Mirrored", Mesh::GetBuiltin(BuiltinMesh::Cube));
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].state.cull, CullMode::Back);

    // One negative scale mirrors (negative determinant)...
    object.gameObject->GetPositionable()->SetLocalScale(Math::Vector3(1.0f, -2.0f, 1.0f));
    renderer.draws.clear();
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].state.cull, CullMode::Front);

    // ...two don't (a rotation)
    object.gameObject->GetPositionable()->SetLocalScale(Math::Vector3(-1.0f, -1.0f, 1.0f));
    renderer.draws.clear();
    object.renderer->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.draws[0].state.cull, CullMode::Back);

    // The rule itself
    Math::Matrix<float, 4, 4> mirror{Math::Matrix<float, 4, 4>::identity()};
    EXPECT_FALSE(Rendering::MeshDrawing::IsMirrored(mirror));
    mirror(2, 2) = -1.0f;
    EXPECT_TRUE(Rendering::MeshDrawing::IsMirrored(mirror));
    object.renderer->OnDestroy();
}

// ============================================================================
// Queues through Scene::Render
// ============================================================================

TEST(MeshRendererQueueTest, OpaqueAndBlendedSubmeshesDrawInTheirOwnQueues)
{
    // Mixed: submesh 0 opaque, submesh 1 blended. A plain opaque cube before it in the hierarchy, a blended quad
    // behind it.
    const auto scene = Scene::Create("MeshRenderer_Queues");
    auto wall = MakeMeshObject("Wall", Mesh::GetBuiltin(BuiltinMesh::Cube), {0, 0, -3});
    auto mixed = MakeMeshObject("Mixed", TwoSubmeshMesh(), {0, 0, -5});
    const auto glassMaterial = MaterialWith(AlphaMode::Blend);
    glassMaterial->SetBaseColor(Common::Color{0.0f, 0.0f, 1.0f, 0.5f});
    mixed.renderer->SetMaterial(0, MaterialWith(AlphaMode::Opaque));
    mixed.renderer->SetMaterial(1, glassMaterial);
    auto farGlass = MakeMeshObject("FarGlass", Mesh::GetBuiltin(BuiltinMesh::Quad), {0, 0, -9});
    farGlass.renderer->SetMaterial(0, glassMaterial);
    scene->AddRootGameObjects({wall.gameObject, mixed.gameObject, farGlass.gameObject});

    EXPECT_TRUE(mixed.renderer->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_TRUE(mixed.renderer->DrawsInQueue(RenderQueue::Transparent));
    EXPECT_EQ(mixed.renderer->GetRenderQueue().queue, RenderQueue::Opaque);
    EXPECT_EQ(farGlass.renderer->GetRenderQueue().queue, RenderQueue::Transparent);
    EXPECT_FALSE(farGlass.renderer->DrawsInQueue(RenderQueue::Opaque));

    RecordingMeshRenderer renderer;
    scene->Render(&renderer, Camera{});
    ASSERT_EQ(renderer.draws.size(), 4u);

    // Opaque, in hierarchy order: the wall, the mixed renderer's opaque submesh
    EXPECT_EQ(renderer.draws[0].range, (IndexRange{0, 36}));
    EXPECT_EQ(renderer.draws[0].state, RenderState::Opaque());
    EXPECT_EQ(renderer.draws[1].range, (IndexRange{0, 6}));
    EXPECT_EQ(renderer.draws[1].state, RenderState::Opaque());
    // Transparent, back to front: the far glass (depth 9), then the mixed renderer's blended submesh (depth 5)
    EXPECT_EQ(renderer.draws[2].range, (IndexRange{0, 6}));
    EXPECT_FLOAT_EQ(renderer.draws[2].model[11], -9.0f);
    EXPECT_EQ(renderer.draws[3].range, (IndexRange{6, 6}));
    EXPECT_FLOAT_EQ(renderer.draws[3].model[11], -5.0f);
    for (std::size_t i = 2; i < 4; ++i)
    {
        EXPECT_EQ(renderer.draws[i].state, RenderState::Transparent()) << i;
        EXPECT_TRUE(renderer.draws[i].state.blend) << "only Blend materials blend";
        EXPECT_FALSE(renderer.draws[i].state.depthWrite);
        EXPECT_FLOAT_EQ(renderer.draws[i].albedo[3], 0.5f);
    }
}

// ============================================================================
// GPU resources: shared through the GpuCache, under the lifetime rules
// ============================================================================

TEST(MeshRendererLifetimeTest, RenderersOfOneMeshShareOneGpuMeshAndMaterial)
{
    RecordingMeshRenderer renderer;
    const auto cube = Mesh::GetBuiltin(BuiltinMesh::Cube);
    auto a = MakeMeshObject("A", cube);
    auto b = MakeMeshObject("B", cube);
    a.renderer->Render(&renderer);
    b.renderer->Render(&renderer);
    a.renderer->Render(&renderer);

    EXPECT_EQ(renderer.GetCounts().createdMeshes, 1);
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, cube.get(), GpuCache::ResourceKind::Mesh), 2u);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, Material::GetDefault().get(), GpuCache::ResourceKind::Material,
                                     Material::GetDefault()->GetGpuVersion()),
              2u);

    a.renderer->OnDestroy();
    EXPECT_EQ(renderer.GetCounts().Destroys(), 0) << "B still uses them";
    EXPECT_EQ(GpuCache::GetUserCount(renderer, cube.get(), GpuCache::ResourceKind::Mesh), 1u);
    b.renderer->OnDestroy();
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 1);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 1);
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
}

TEST(MeshRendererLifetimeTest, ARendererRecreatedAtTheSameAddressGetsNewResources)
{
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    auto object = MakeMeshObject("Recreated", Mesh::GetBuiltin(BuiltinMesh::Quad));
    object.renderer->Render(&*renderer);
    ASSERT_EQ(counts.createdMeshes, 1);

    renderer.reset();         // frees everything it made
    renderer.emplace(counts); // same storage, so the same address
    object.renderer->Render(&*renderer);
    EXPECT_EQ(counts.createdMeshes, 2);
    EXPECT_EQ(counts.createdMaterials, 2);
    EXPECT_EQ(counts.Destroys(), 0) << "nothing of the old renderer's is destroyed on the new one";
    ASSERT_EQ(renderer->draws.size(), 1u);
    EXPECT_TRUE(renderer->OwnsMesh(renderer->draws[0].mesh));
    EXPECT_TRUE(renderer->OwnsMaterial(renderer->draws[0].material));

    object.renderer->CleanupRenderResources(&*renderer);
    EXPECT_TRUE(renderer->meshes.empty());
    EXPECT_TRUE(renderer->materials.empty());
}

TEST(MeshRendererLifetimeTest, ChangingTheMeshOrTheMaterialSwapsTheShares)
{
    RecordingMeshRenderer renderer;
    const auto mesh = Mesh::Create(Mesh::MakeQuad());
    const auto material = MaterialWith(AlphaMode::Opaque);
    auto object = MakeMeshObject("Changing", mesh);
    object.renderer->SetMaterial(0, material);
    object.renderer->Render(&renderer);

    // The mesh's geometry changes: re-uploaded, not recreated
    ASSERT_TRUE(mesh->SetData(Mesh::MakeCube()));
    object.renderer->Render(&renderer);
    EXPECT_EQ(renderer.GetCounts().updatedMeshes, 1);
    EXPECT_EQ(renderer.draws.back().range, (IndexRange{0, 36}));

    // The material's colour changes, 100 times: the same GPU material, the colour set per draw
    for (int frame = 0; frame < 100; ++frame)
    {
        material->SetBaseColor(Common::Color{0.0f, static_cast<float>(frame) / 99.0f, 0.0f, 1.0f});
        object.renderer->Render(&renderer);
    }
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 0);
    EXPECT_FLOAT_EQ(renderer.draws.back().albedo[1], 1.0f);
    EXPECT_FLOAT_EQ(renderer.draws.back().albedo[0], 0.0f);

    // Its shader changes: a new GPU material for the new GPU version, the old one released
    material->SetShading(ShadingModel::Lit);
    object.renderer->Render(&renderer);
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 2);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 1);
    EXPECT_EQ(RecordingMeshRenderer::AsSW(renderer.draws.back().material)->GetShader(), &renderer.litShader);

    // Another mesh: the old one's share goes (its last user, so it is destroyed)
    object.renderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Quad));
    object.renderer->Render(&renderer);
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 1);
    EXPECT_EQ(renderer.meshes.size(), 1u);
    object.renderer->OnDestroy();
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
}

TEST(MeshRendererLifetimeTest, DestroyingAfterTheRendererIsGoneCallsNothing)
{
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    auto object = MakeMeshObject("Orphaned", Mesh::GetBuiltin(BuiltinMesh::Cube));
    object.renderer->Render(&*renderer);

    renderer->Shutdown(); // its resources are gone
    object.renderer->OnDestroy();
    EXPECT_EQ(counts.Destroys(), 0);
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, Mesh::GetBuiltin(BuiltinMesh::Cube).get(),
                                     GpuCache::ResourceKind::Mesh),
              0u);
}

// ============================================================================
// Serialization
// ============================================================================

TEST(MeshRendererSerializationTest, TheMeshAndMaterialsSurviveASceneLoad)
{
    ASSERT_TRUE(ComponentRegistry::Instance().IsRegistered("MeshRenderer"));
    const auto material = MaterialWith(AlphaMode::Mask);
    IO::Resources::Instance().RegisterAsset(material); // found by its (per-run) UUID

    const auto scene = Scene::Create("MeshRenderer_RoundTrip");
    auto object = MakeMeshObject("Saved", Mesh::GetBuiltin(BuiltinMesh::Sphere));
    object.renderer->SetMaterials({nullptr, material});
    scene->AddRootGameObject(object.gameObject);

    const auto loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto loadedObject = loaded->FindGameObject("Saved");
    ASSERT_NE(loadedObject, nullptr);
    const auto *loadedRenderer = loadedObject->GetComponent<MeshRenderer>();
    ASSERT_NE(loadedRenderer, nullptr);
    EXPECT_EQ(loadedRenderer->GetMesh(), Mesh::GetBuiltin(BuiltinMesh::Sphere)) << "by its fixed UUID";
    ASSERT_EQ(loadedRenderer->GetMaterials().size(), 2u);
    EXPECT_EQ(loadedRenderer->GetMaterials()[0], nullptr);
    EXPECT_EQ(loadedRenderer->GetMaterials()[1], material);

    IO::Resources::Instance().UnregisterAsset(material->GetUUID());
}

TEST(MeshRendererSerializationTest, NoMeshSavesNull)
{
    const auto go = GameObject::Create("Bare");
    auto *renderer = go->AddComponent<MeshRenderer>();
    const auto saved = renderer->Serialize();
    EXPECT_TRUE(saved.at("_mesh").is_null());
    EXPECT_TRUE(saved.at("_materials").is_array());
    EXPECT_TRUE(saved.at("_materials").empty());
}
