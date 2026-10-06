#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWShader.hpp>

#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"

#include "MeshTestSupport.hpp"

// GpuCache's meshes and materials: one IMesh per (renderer, Mesh), re-uploaded when the mesh changes, and one
// IMaterial per (renderer, Material, version) holding a share of its texture, under the same lifetime-token rules as
// textures. The cache is process-wide, so counts are compared with what was there when each test began.

using namespace N2Engine;
using Rendering::GpuCache;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::ShadingModel;
using Rendering::Texture;
using MeshTestSupport::Counts;
using MeshTestSupport::RecordingMeshRenderer;

namespace
{
    std::shared_ptr<Mesh> MakeMesh()
    {
        return Mesh::Create(Mesh::MakeQuad());
    }

    std::shared_ptr<Texture> MakeTexture()
    {
        return Texture::Create(1, 1, std::vector<std::uint8_t>{10, 20, 30, 255});
    }
}

// ============================================================================
// Meshes
// ============================================================================

TEST(GpuCacheMeshTest, OneGpuMeshPerRendererAndMeshSharedByEveryHandle)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    const auto mesh = MakeMesh();

    GpuCache::Handle first = GpuCache::AcquireMesh(renderer, mesh);
    GpuCache::Handle second = GpuCache::AcquireMesh(renderer, mesh);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.GetKind(), GpuCache::ResourceKind::Mesh);
    EXPECT_EQ(first.GetMesh(), second.GetMesh());
    EXPECT_EQ(first.GetTexture(), nullptr);
    EXPECT_EQ(first.GetMaterial(), nullptr);
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, mesh.get(), GpuCache::ResourceKind::Mesh), 2u);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 1);
    ASSERT_EQ(renderer.meshes.size(), 1u);
    EXPECT_EQ(renderer.meshes[0]->indices, mesh->GetIndices());

    first.Release(true);
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 0) << "the other handle still uses it";
    second.Release(true);
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 1) << "the last release destroys it";
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheMeshTest, TwoRenderersEachGetTheirOwnGpuMesh)
{
    RecordingMeshRenderer a;
    RecordingMeshRenderer b;
    const auto mesh = MakeMesh();
    GpuCache::Handle onA = GpuCache::AcquireMesh(a, mesh);
    GpuCache::Handle onB = GpuCache::AcquireMesh(b, mesh);
    EXPECT_EQ(a.GetCounts().createdMeshes, 1);
    EXPECT_EQ(b.GetCounts().createdMeshes, 1);
    EXPECT_TRUE(a.OwnsMesh(onA.GetMesh()));
    EXPECT_TRUE(b.OwnsMesh(onB.GetMesh()));
    EXPECT_EQ(GpuCache::GetUserCount(a, mesh.get(), GpuCache::ResourceKind::Mesh), 1u);
    EXPECT_EQ(GpuCache::GetUserCount(b, mesh.get(), GpuCache::ResourceKind::Mesh), 1u);
    onA.Release(true);
    onB.Release(true);
    EXPECT_EQ(a.GetCounts().destroyedMeshes, 1);
    EXPECT_EQ(b.GetCounts().destroyedMeshes, 1);
}

TEST(GpuCacheMeshTest, AChangedMeshIsReuploadedInPlace)
{
    RecordingMeshRenderer renderer;
    const auto mesh = MakeMesh();
    GpuCache::Handle handle = GpuCache::AcquireMesh(renderer, mesh);
    Renderer::Common::IMesh *gpu = handle.GetMesh();

    EXPECT_TRUE(GpuCache::SyncMesh(handle));
    EXPECT_EQ(renderer.GetCounts().updatedMeshes, 0) << "unchanged: nothing to upload";

    ASSERT_TRUE(mesh->SetData(Mesh::MakeCube()));
    EXPECT_TRUE(GpuCache::SyncMesh(handle));
    EXPECT_EQ(renderer.GetCounts().updatedMeshes, 1);
    EXPECT_EQ(handle.GetMesh(), gpu) << "updated in place";
    EXPECT_EQ(renderer.meshes[0]->indices.size(), 36u);
    EXPECT_TRUE(GpuCache::SyncMesh(handle));
    EXPECT_EQ(renderer.GetCounts().updatedMeshes, 1) << "once per change";

    // A new acquire after a change sees the new geometry too
    ASSERT_TRUE(mesh->SetData(Mesh::MakeQuad()));
    GpuCache::Handle other = GpuCache::AcquireMesh(renderer, mesh);
    EXPECT_EQ(renderer.GetCounts().updatedMeshes, 2);
    EXPECT_EQ(renderer.meshes[0]->indices.size(), 6u);
    handle.Release(true);
    other.Release(true);
}

TEST(GpuCacheMeshTest, ARendererThatCantUpdateMeshesGetsANewOne)
{
    RecordingMeshRenderer renderer;
    renderer.canUpdateMeshes = false;
    const auto mesh = MakeMesh();
    GpuCache::Handle first = GpuCache::AcquireMesh(renderer, mesh);
    GpuCache::Handle second = GpuCache::AcquireMesh(renderer, mesh);
    Renderer::Common::IMesh *old = first.GetMesh();

    ASSERT_TRUE(mesh->SetData(Mesh::MakeCube()));
    EXPECT_TRUE(GpuCache::SyncMesh(first));
    EXPECT_NE(first.GetMesh(), old);
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 2);
    EXPECT_EQ(renderer.GetCounts().destroyedMeshes, 1) << "the old copy is gone";
    EXPECT_TRUE(renderer.OwnsMesh(first.GetMesh()));

    // The other user's handle is repointed by its own sync, without uploading again
    EXPECT_TRUE(GpuCache::SyncMesh(second));
    EXPECT_EQ(second.GetMesh(), first.GetMesh());
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 2);
    first.Release(true);
    second.Release(true);
    EXPECT_TRUE(renderer.meshes.empty());
}

TEST(GpuCacheMeshTest, ADestroyedRendererIsNeverCalledAndItsEntryIsReplaced)
{
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    const auto mesh = MakeMesh();
    GpuCache::Handle handle = GpuCache::AcquireMesh(*renderer, mesh);
    ASSERT_TRUE(handle);

    renderer.reset();         // frees everything it made
    renderer.emplace(counts); // a new renderer at the same address
    EXPECT_FALSE(handle.Holds(&*renderer));
    EXPECT_FALSE(GpuCache::SyncMesh(handle));
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, mesh.get(), GpuCache::ResourceKind::Mesh), 0u);

    GpuCache::Handle fresh = GpuCache::AcquireMesh(*renderer, mesh);
    EXPECT_EQ(counts.createdMeshes, 2) << "the stale entry is replaced";
    handle.Release(true);
    EXPECT_EQ(counts.destroyedMeshes, 0) << "the stale handle never destroys the new renderer's mesh";
    EXPECT_EQ(GpuCache::GetUserCount(*renderer, mesh.get(), GpuCache::ResourceKind::Mesh), 1u);
    fresh.Release(true);
    EXPECT_EQ(counts.destroyedMeshes, 1);
}

TEST(GpuCacheMeshTest, NullOrEmptyMeshesAcquireNothing)
{
    RecordingMeshRenderer renderer;
    EXPECT_FALSE(GpuCache::AcquireMesh(renderer, nullptr));
    EXPECT_FALSE(GpuCache::AcquireMesh(renderer, std::make_shared<Mesh>()));
    EXPECT_EQ(renderer.GetCounts().createdMeshes, 0);
}

// ============================================================================
// Materials
// ============================================================================

TEST(GpuCacheMaterialTest, OneGpuMaterialPerRendererMaterialAndVersion)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    const auto material = Material::Create(ShadingModel::Lit);
    material->SetBaseColor(Common::Color{0.5f, 0.25f, 1.0f, 1.0f});

    GpuCache::Handle first = GpuCache::AcquireMaterial(renderer, material);
    GpuCache::Handle second = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.GetKind(), GpuCache::ResourceKind::Material);
    EXPECT_EQ(first.GetMaterial(), second.GetMaterial());
    EXPECT_EQ(first.GetVersion(), material->GetVersion());
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, material.get(), GpuCache::ResourceKind::Material, material->GetVersion()),
              2u);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 1);

    // The lit shader, and the material's uniforms
    const auto *sw = RecordingMeshRenderer::AsSW(first.GetMaterial());
    ASSERT_NE(sw, nullptr);
    EXPECT_EQ(sw->GetShader(), &renderer.litShader);
    EXPECT_FLOAT_EQ(sw->GetVec4("uAlbedo")[1], 0.25f);

    // A change makes a new version: a separate GPU material, the old one going with its last user
    const std::uint64_t oldVersion = material->GetVersion();
    material->SetShading(ShadingModel::Unlit);
    GpuCache::Handle changed = GpuCache::AcquireMaterial(renderer, material);
    EXPECT_NE(changed.GetMaterial(), first.GetMaterial());
    EXPECT_EQ(RecordingMeshRenderer::AsSW(changed.GetMaterial())->GetShader(), &renderer.unlitShader);
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 2);
    first.Release(true);
    second.Release(true);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 1);
    EXPECT_EQ(GpuCache::GetUserCount(renderer, material.get(), GpuCache::ResourceKind::Material, oldVersion), 0u);
    changed.Release(true);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 2);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheMaterialTest, TheMaterialHoldsAShareOfItsTexture)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    const auto texture = MakeTexture();
    const auto material = Material::Create(ShadingModel::Unlit);
    material->SetBaseColorTexture(texture);

    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_EQ(renderer.GetCounts().createdTextures, 1);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 2) << "the material's entry and its texture's";
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 1u) << "the material entry's share";
    ASSERT_EQ(renderer.textures.size(), 1u);
    EXPECT_EQ(handle.GetMaterial()->GetTexture(), renderer.textures[0].get());
    EXPECT_EQ(RecordingMeshRenderer::AsSW(handle.GetMaterial())->GetVec4("uAlbedo")[0], 1.0f);

    // An Image or another material using the same texture shares it
    GpuCache::Handle direct = GpuCache::AcquireTexture(renderer, texture);
    EXPECT_EQ(direct.GetTexture(), renderer.textures[0].get());
    EXPECT_EQ(GpuCache::GetUserCount(renderer, texture.get()), 2u);
    direct.Release(true);

    // The last user of the material destroys it, then its texture
    handle.Release(true);
    EXPECT_EQ(renderer.GetCounts().destroyedMaterials, 1);
    EXPECT_EQ(renderer.GetCounts().destroyedTextures, 1);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCacheMaterialTest, ADestroyedRendererLetsGoOfTheTextureShareWithoutCallingIt)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    Counts counts;
    std::optional<RecordingMeshRenderer> renderer;
    renderer.emplace(counts);
    const auto material = Material::Create(ShadingModel::Unlit);
    material->SetBaseColorTexture(MakeTexture());
    GpuCache::Handle handle = GpuCache::AcquireMaterial(*renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 2);

    renderer.reset();
    handle.Release(true);
    EXPECT_EQ(counts.destroyedMaterials, 0);
    EXPECT_EQ(counts.destroyedTextures, 0);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline) << "both entries are gone";
}

TEST(GpuCacheMaterialTest, ARendererWithoutTheShaderMakesNothing)
{
    RecordingMeshRenderer renderer;
    renderer.hasLitShader = false;
    const auto material = Material::Create(ShadingModel::Lit);
    material->SetBaseColorTexture(MakeTexture());
    EXPECT_FALSE(GpuCache::AcquireMaterial(renderer, material));
    EXPECT_FALSE(GpuCache::AcquireMaterial(renderer, nullptr));
    EXPECT_EQ(renderer.GetCounts().createdMaterials, 0);
    EXPECT_EQ(renderer.GetCounts().createdTextures, 0) << "no shader: the texture isn't made either";
}
