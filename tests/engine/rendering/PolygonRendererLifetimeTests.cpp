#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"

// PolygonRenderer (CubeRenderer, QuadRenderer) identifies its renderer by address and lifetime token, so
// a renderer recreated at the same address, or a switch to another renderer, gets new resources. Drawn
// through a renderer that keeps CPU-side resources and records every call; no window or GPU is involved.

using namespace N2Engine;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;

namespace
{
    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        // What was asked for
        std::vector<IMesh *> drawnMeshes;
        std::vector<IMaterial *> drawnMaterials;
        int createMeshCalls = 0;
        int destroyMeshCalls = 0;
        int createMaterialCalls = 0;
        int destroyMaterialCalls = 0;
        int destroyShaderCalls = 0;

        // What is alive
        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        Renderer::Software::SWShader unlitShader{Renderer::Software::SWShaderType::Unlit};

        IMesh *CreateMesh(const MeshData &meshData) override
        {
            ++createMeshCalls;
            auto mesh = std::make_unique<Renderer::Software::SWMesh>();
            mesh->vertices = meshData.vertices;
            mesh->indices = meshData.indices;
            meshes.push_back(std::move(mesh));
            return meshes.back().get();
        }

        void DestroyMesh(IMesh *mesh) override
        {
            ++destroyMeshCalls;
            std::erase_if(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }

        IMaterial *CreateMaterial(IShader *shader) override { return CreateMaterial(shader, nullptr); }

        IMaterial *CreateMaterial(IShader *shader, ITexture *texture) override
        {
            ++createMaterialCalls;
            materials.push_back(std::make_unique<Renderer::Software::SWMaterial>(shader, texture));
            return materials.back().get();
        }

        void DestroyMaterial(IMaterial *material) override
        {
            ++destroyMaterialCalls;
            std::erase_if(materials, [material](const auto &p) { return p.get() == material; });
        }

        bool DestroyShaderProgram(IShader *) override
        {
            ++destroyShaderCalls;
            return true;
        }

        [[nodiscard]] IShader *GetStandardUnlitShader() const override
        {
            return const_cast<Renderer::Software::SWShader *>(&unlitShader);
        }

        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *mesh, const float *, IMaterial *material, const RenderState &) override
        {
            drawnMeshes.push_back(mesh);
            drawnMaterials.push_back(material);
        }

        [[nodiscard]] bool OwnsMesh(const IMesh *mesh) const
        {
            return std::ranges::any_of(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }
        [[nodiscard]] bool OwnsMaterial(const IMaterial *material) const
        {
            return std::ranges::any_of(materials, [material](const auto &p) { return p.get() == material; });
        }
        [[nodiscard]] int DestroyCalls() const { return destroyMeshCalls + destroyMaterialCalls + destroyShaderCalls; }

        ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override { return nullptr; }
        void DestroyTexture(ITexture *) override {}
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(IShader *) override {}
        bool IsValidShader(IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }
    };

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
}

TEST(PolygonRendererLifetimeTest, TheSameRendererKeepsItsResources)
{
    RecordingRenderer renderer;
    auto object = MakePolygon<Example::CubeRenderer>("Kept");

    object.renderer->Render(&renderer);
    object.renderer->Render(&renderer);

    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.createMaterialCalls, 1);
    EXPECT_EQ(renderer.DestroyCalls(), 0);
    ASSERT_EQ(renderer.drawnMeshes.size(), 2u);
    EXPECT_EQ(renderer.drawnMeshes[0], renderer.drawnMeshes[1]);

    object.renderer->CleanupRenderResources(&renderer);
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
}

TEST(PolygonRendererLifetimeTest, ARendererRecreatedAtTheSameAddressGetsNewResources)
{
    // Window recreates its renderer, and the new one can land where the old one was. The component
    // isn't told, so it must not take the new renderer for the old one.
    std::optional<RecordingRenderer> renderer;
    renderer.emplace();
    auto object = MakePolygon<Example::CubeRenderer>("Recreated");
    object.renderer->Render(&*renderer);
    ASSERT_EQ(renderer->createMeshCalls, 1);

    renderer.reset();   // frees everything it made
    renderer.emplace(); // same storage, so the same address

    object.renderer->Render(&*renderer);
    EXPECT_EQ(renderer->createMeshCalls, 1); // a new mesh and material, made on the new renderer
    EXPECT_EQ(renderer->createMaterialCalls, 1);
    EXPECT_EQ(renderer->DestroyCalls(), 0); // nothing of the old renderer's is destroyed here
    ASSERT_EQ(renderer->drawnMeshes.size(), 1u);
    EXPECT_TRUE(renderer->OwnsMesh(renderer->drawnMeshes[0]));
    EXPECT_TRUE(renderer->OwnsMaterial(renderer->drawnMaterials[0]));

    object.renderer->CleanupRenderResources(&*renderer);
    EXPECT_TRUE(renderer->meshes.empty());
    EXPECT_TRUE(renderer->materials.empty());
}

TEST(PolygonRendererLifetimeTest, ANewRendererReleasesWhatTheOldOneHeld)
{
    RecordingRenderer first;
    RecordingRenderer second;
    auto object = MakePolygon<Example::QuadRenderer>("Switch");

    object.renderer->Render(&first);
    object.renderer->Render(&second);

    EXPECT_TRUE(first.meshes.empty());
    EXPECT_TRUE(first.materials.empty());
    EXPECT_EQ(first.destroyMeshCalls, 1);
    EXPECT_EQ(first.destroyMaterialCalls, 1);
    EXPECT_EQ(second.createMeshCalls, 1);
    EXPECT_EQ(second.DestroyCalls(), 0);
    ASSERT_EQ(second.drawnMeshes.size(), 1u);
    EXPECT_TRUE(second.OwnsMesh(second.drawnMeshes[0]));

    // Cleanup on a renderer it holds nothing on does nothing
    object.renderer->CleanupRenderResources(&first);
    EXPECT_EQ(second.DestroyCalls(), 0);
    EXPECT_EQ(first.destroyMeshCalls, 1);

    object.renderer->CleanupRenderResources(&second);
    EXPECT_TRUE(second.meshes.empty());
    EXPECT_TRUE(second.materials.empty());
}

TEST(PolygonRendererLifetimeTest, DestroyingAfterTheRendererIsGoneCallsNothing)
{
    std::optional<RecordingRenderer> renderer;
    renderer.emplace();
    auto object = MakePolygon<Example::CubeRenderer>("Orphaned");
    object.renderer->Render(&*renderer);

    renderer.reset();
    renderer.emplace(); // a new renderer where the old one was

    // The resources died with the old renderer, so they are forgotten, not destroyed on the new one
    object.renderer->OnDestroy();
    EXPECT_EQ(renderer->DestroyCalls(), 0);

    // Drawing again makes new ones
    object.renderer->Render(&*renderer);
    EXPECT_EQ(renderer->createMeshCalls, 1);
    EXPECT_EQ(renderer->DestroyCalls(), 0);
    object.renderer->CleanupRenderResources(&*renderer);
    EXPECT_TRUE(renderer->meshes.empty());
}
