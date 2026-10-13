#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/rendering/RenderCamera.hpp"

// ParticleSystem through a renderer that keeps CPU-side resources and records every call: what it draws, with which
// state, how the one mesh is kept up to date, and what it releases. No window or GPU is involved.

using namespace N2Engine;
using Rendering::ParticleSystem;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;

namespace
{
    struct RecordedDraw
    {
        IMesh *mesh = nullptr;
        std::array<float, 16> model{};
        IMaterial *material = nullptr;
        RenderState state;
        std::size_t vertexCount = 0;
        std::vector<Renderer::Common::Vertex> vertices; // the mesh's contents when it was drawn
    };

    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        bool supportsUpdateMesh = true;
        bool hasUnlitShader = true;

        std::vector<RecordedDraw> draws;
        int createMeshCalls = 0;
        int updateMeshCalls = 0;
        int destroyMeshCalls = 0;
        int createMaterialCalls = 0;
        int destroyMaterialCalls = 0;

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

        bool UpdateMesh(IMesh *mesh, const MeshData &meshData) override
        {
            if (!supportsUpdateMesh)
            {
                return false;
            }
            ++updateMeshCalls;
            auto *sw = Find(meshes, mesh);
            if (!sw || meshData.vertices.empty())
            {
                return false;
            }
            sw->vertices = meshData.vertices;
            sw->indices = meshData.indices;
            return true;
        }

        void DestroyMesh(IMesh *mesh) override
        {
            ++destroyMeshCalls;
            std::erase_if(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }

        ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override { return nullptr; }
        void DestroyTexture(ITexture *) override {}

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

        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material, const RenderState &state) override
        {
            RecordedDraw draw;
            draw.mesh = mesh;
            std::copy_n(modelMatrix, 16, draw.model.begin());
            draw.material = material;
            draw.state = state;
            if (const auto *sw = Find(meshes, mesh))
            {
                draw.vertexCount = sw->vertices.size();
                draw.vertices = sw->vertices;
            }
            draws.push_back(std::move(draw));
        }

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
        bool DestroyShaderProgram(IShader *) override { return false; }
        bool IsValidShader(IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] IShader *GetStandardUnlitShader() const override
        {
            return hasUnlitShader ? const_cast<Renderer::Software::SWShader *>(&unlitShader) : nullptr;
        }
        [[nodiscard]] IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }

    private:
        template <typename Owned, typename Base>
        static Owned *Find(const std::vector<std::unique_ptr<Owned>> &owned, Base *object)
        {
            const auto it = std::ranges::find_if(owned, [object](const auto &p) { return p.get() == object; });
            return it == owned.end() ? nullptr : it->get();
        }
    };

    struct Fx
    {
        GameObject::Ptr gameObject;
        ParticleSystem *system = nullptr;
    };

    /// A started CPU system with no stream: tests emit bursts
    Fx MakeFx(const std::string &name)
    {
        Fx fx;
        fx.gameObject = GameObject::Create(name);
        fx.system = fx.gameObject->AddComponent<ParticleSystem>();
        fx.system->backend = Particles::ParticleBackend::CPU;
        fx.system->playOnAwake = false;
        fx.system->lifetimeMin = 100.0f;
        fx.system->lifetimeMax = 100.0f;
        fx.system->speedMin = 0.0f;
        fx.system->speedMax = 0.0f;
        fx.system->gravityScale = 0.0f;
        fx.system->OnAttach();
        return fx;
    }
}

TEST(ParticleRendererTest, IsATransparentRenderableWithNoBoundsOfItsOwn)
{
    const Fx fx = MakeFx("ParticleQueue");
    const IRenderable *renderable = fx.system;
    EXPECT_EQ(renderable->GetRenderQueue().queue, RenderQueue::Transparent);
    EXPECT_TRUE(renderable->DrawsInQueue(RenderQueue::Transparent));
    EXPECT_FALSE(renderable->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_FALSE(renderable->GetWorldBounds().has_value()) << "picked by a small sphere around its position";
}

TEST(ParticleRendererTest, DrawsNothingWithoutParticlesOrBeforeTheSystemStarted)
{
    RecordingRenderer renderer;

    const auto object = GameObject::Create("ParticleNotStarted");
    auto *notStarted = object->AddComponent<ParticleSystem>();
    notStarted->Render(&renderer);
    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_EQ(renderer.createMeshCalls, 0);

    const Fx fx = MakeFx("ParticleEmpty");
    fx.system->Render(&renderer);
    EXPECT_TRUE(renderer.draws.empty()) << "a started system with no particles has nothing to draw";
    EXPECT_EQ(renderer.createMeshCalls, 0);
    EXPECT_EQ(renderer.createMaterialCalls, 0);

    fx.system->Render(nullptr); // no renderer: no crash
}

TEST(ParticleRendererTest, DrawsOneMeshWithAQuadPerParticleInTheTransparentState)
{
    RecordingRenderer renderer;
    const Fx fx = MakeFx("ParticleDraw");
    fx.system->EmitBurst(6);
    fx.system->Render(&renderer);

    ASSERT_EQ(renderer.draws.size(), 1u);
    const RecordedDraw &draw = renderer.draws[0];
    EXPECT_EQ(draw.vertexCount, 24u) << "4 vertices a particle";
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.createMaterialCalls, 1);
    EXPECT_TRUE(draw.state.blend);
    EXPECT_FALSE(draw.state.depthWrite) << "the Transparent queue's state";
    EXPECT_TRUE(draw.state.depthTest);
    EXPECT_EQ(draw.state.cull, Renderer::Common::CullMode::None);
    EXPECT_FALSE(draw.state.additive);
    for (const float m : {draw.model[0], draw.model[5], draw.model[10], draw.model[15]})
    {
        EXPECT_FLOAT_EQ(m, 1.0f) << "world-space quads: an identity model matrix";
    }
    EXPECT_FLOAT_EQ(draw.model[12], 0.0f);

    fx.system->CleanupRenderResources(&renderer);
}

TEST(ParticleRendererTest, AdditiveBlendReachesTheDrawState)
{
    RecordingRenderer renderer;
    const Fx fx = MakeFx("ParticleAdditive");
    fx.system->blendMode = Rendering::ParticleBlend::Additive;
    fx.system->EmitBurst(2);
    fx.system->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    EXPECT_TRUE(renderer.draws[0].state.blend);
    EXPECT_TRUE(renderer.draws[0].state.additive);

    fx.system->blendMode = Rendering::ParticleBlend::Alpha;
    fx.system->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 2u);
    EXPECT_FALSE(renderer.draws[1].state.additive);

    fx.system->CleanupRenderResources(&renderer);
}

TEST(ParticleRendererTest, TheMeshIsUpdatedInPlaceEachFrameAndTheMaterialIsMadeOnce)
{
    RecordingRenderer renderer;
    const Fx fx = MakeFx("ParticleInPlace");
    fx.system->EmitBurst(3);
    fx.system->Render(&renderer);
    fx.system->EmitBurst(4);
    fx.system->Render(&renderer);
    fx.system->Render(&renderer);

    ASSERT_EQ(renderer.draws.size(), 3u);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 2);
    EXPECT_EQ(renderer.destroyMeshCalls, 0);
    EXPECT_EQ(renderer.createMaterialCalls, 1);
    EXPECT_EQ(renderer.draws[0].mesh, renderer.draws[2].mesh) << "the same IMesh throughout";
    EXPECT_EQ(renderer.draws[0].material, renderer.draws[2].material);
    EXPECT_EQ(renderer.draws[0].vertexCount, 12u);
    EXPECT_EQ(renderer.draws[1].vertexCount, 28u);

    fx.system->CleanupRenderResources(&renderer);
}

TEST(ParticleRendererTest, ABackendThatCannotUpdateMeshesGetsANewMeshEachFrame)
{
    RecordingRenderer renderer;
    renderer.supportsUpdateMesh = false;
    const Fx fx = MakeFx("ParticleNoUpdate");
    fx.system->EmitBurst(3);
    fx.system->Render(&renderer);
    fx.system->Render(&renderer);

    ASSERT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(renderer.createMeshCalls, 2);
    EXPECT_EQ(renderer.destroyMeshCalls, 1) << "the old one is destroyed, not leaked";
    EXPECT_EQ(renderer.meshes.size(), 1u);

    fx.system->CleanupRenderResources(&renderer);
    EXPECT_TRUE(renderer.meshes.empty());
}

TEST(ParticleRendererTest, ARendererWithoutTheUnlitShaderDrawsNothing)
{
    RecordingRenderer renderer;
    renderer.hasUnlitShader = false;
    const Fx fx = MakeFx("ParticleNoShader");
    fx.system->EmitBurst(3);
    EXPECT_NO_THROW(fx.system->Render(&renderer));
    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_EQ(renderer.createMeshCalls, 0);
    EXPECT_EQ(renderer.createMaterialCalls, 0);
}

TEST(ParticleRendererTest, QuadsFaceTheCameraOfTheSceneRenderThatDrawsThem)
{
    RecordingRenderer renderer;
    const Fx fx = MakeFx("ParticleFacing");
    fx.system->startSize = 2.0f;
    fx.system->endSize = 2.0f;
    fx.system->EmitBurst(1);

    // Default basis (outside a Scene::Render): right +X, up +Y
    fx.system->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    const auto &flat = renderer.draws[0].vertices;
    ASSERT_EQ(flat.size(), 4u);
    EXPECT_FLOAT_EQ(flat[1].position[0] - flat[0].position[0], 2.0f);
    EXPECT_FLOAT_EQ(flat[1].position[2], flat[0].position[2]);

    // A camera turned a quarter about Y looks down -X: its right is -Z... whatever it is, the quad lies in the plane
    // perpendicular to the camera's forward and is as big
    Camera camera;
    camera.SetPosition(Math::Vector3(5.0f, 0.0f, 0.0f));
    camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
    {
        const Rendering::RenderCamera::Scope scope(camera);
        fx.system->Render(&renderer);
    }
    ASSERT_EQ(renderer.draws.size(), 2u);
    const auto &turned = renderer.draws[1].vertices;
    ASSERT_EQ(turned.size(), 4u);
    const Math::Vector3 forward = camera.GetForward();
    for (const auto &vertex : turned)
    {
        const float along = vertex.position[0] * forward.x + vertex.position[1] * forward.y +
                            vertex.position[2] * forward.z;
        EXPECT_NEAR(along, 0.0f, 1e-4f) << "in the plane through the particle, facing the camera";
    }
    const float edge = std::abs(turned[1].position[0] - turned[0].position[0]) +
                       std::abs(turned[1].position[1] - turned[0].position[1]) +
                       std::abs(turned[1].position[2] - turned[0].position[2]);
    EXPECT_NEAR(edge, 2.0f, 1e-3f);

    // The scope ended: the default basis is back
    fx.system->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 3u);
    EXPECT_FLOAT_EQ(renderer.draws[2].vertices[1].position[0] - renderer.draws[2].vertices[0].position[0], 2.0f);

    fx.system->CleanupRenderResources(&renderer);
}

TEST(ParticleRendererTest, ParticlesAreDrawnAtTheirWorldPositionsWhateverTheObjectDoesNext)
{
    RecordingRenderer renderer;
    const Fx fx = MakeFx("ParticleWorld");
    fx.system->startSize = 0.0f;
    fx.system->endSize = 0.0f;
    fx.gameObject->GetPositionable()->SetPosition(Math::Vector3(1.0f, 2.0f, 3.0f));
    fx.system->EmitBurst(1);
    fx.gameObject->GetPositionable()->SetPosition(Math::Vector3(50.0f, 0.0f, 0.0f)); // moves on: the particle stays
    fx.system->Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    const auto &v = renderer.draws[0].vertices[0];
    EXPECT_NEAR(v.position[0], 1.0f, 1e-4f);
    EXPECT_NEAR(v.position[1], 2.0f, 1e-4f);
    EXPECT_NEAR(v.position[2], 3.0f, 1e-4f);

    fx.system->CleanupRenderResources(&renderer);
}

TEST(ParticleRendererTest, ReleasesTheMeshAndMaterialOnCleanupAndOnDestroy)
{
    RecordingRenderer renderer;
    {
        const Fx fx = MakeFx("ParticleCleanup");
        fx.system->EmitBurst(2);
        fx.system->Render(&renderer);
        ASSERT_EQ(renderer.meshes.size(), 1u);
        ASSERT_EQ(renderer.materials.size(), 1u);

        fx.system->CleanupRenderResources(&renderer);
        EXPECT_TRUE(renderer.meshes.empty());
        EXPECT_TRUE(renderer.materials.empty());

        // Drawn again after the cleanup: fresh resources
        fx.system->Render(&renderer);
        EXPECT_EQ(renderer.meshes.size(), 1u);
        EXPECT_EQ(renderer.materials.size(), 1u);

        fx.system->OnDestroy();
        EXPECT_TRUE(renderer.meshes.empty()) << "destroying the component releases what it made";
        EXPECT_TRUE(renderer.materials.empty());
    }
    EXPECT_EQ(renderer.createMeshCalls, 2);
}

TEST(ParticleRendererTest, CleanupOnAnotherRendererLeavesTheBoundOneAlone)
{
    RecordingRenderer first;
    RecordingRenderer second;
    const Fx fx = MakeFx("ParticleTwoRenderers");
    fx.system->EmitBurst(2);
    fx.system->Render(&first);
    ASSERT_EQ(first.meshes.size(), 1u);

    fx.system->CleanupRenderResources(&second);
    EXPECT_EQ(first.meshes.size(), 1u) << "it holds nothing on `second`";

    // Moving to another renderer releases what was held on the first
    fx.system->Render(&second);
    EXPECT_TRUE(first.meshes.empty());
    EXPECT_EQ(second.meshes.size(), 1u);

    fx.system->CleanupRenderResources(&second);
}
