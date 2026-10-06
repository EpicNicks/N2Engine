#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"

// Scene::Render's draw order and per-draw state, through a renderer that only records what it is asked
// to draw. No window or GPU is involved.

using namespace N2Engine;
using Renderer::Common::CullMode;
using Renderer::Common::RenderState;

namespace
{
    struct RecordedDraw
    {
        int id;
        RenderState state;
    };

    // Records every DrawMesh: the drawing renderable's id (carried in the model matrix) and the state
    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        std::vector<RecordedDraw> draws;

        using IRenderer::DrawMesh;
        void DrawMesh(Renderer::Common::IMesh *, const float *modelMatrix, Renderer::Common::IMaterial *,
                      const RenderState &state) override
        {
            draws.push_back(RecordedDraw{static_cast<int>(modelMatrix[0]), state});
        }
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}

        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return false; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return false; }
        Renderer::Common::IMesh *CreateMesh(const Renderer::Common::MeshData &) override { return nullptr; }
        void DestroyMesh(Renderer::Common::IMesh *) override {}
        Renderer::Common::ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override
        {
            return nullptr;
        }
        void DestroyTexture(Renderer::Common::ITexture *) override {}
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *) override { return nullptr; }
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *, Renderer::Common::ITexture *) override
        {
            return nullptr;
        }
        void DestroyMaterial(Renderer::Common::IMaterial *) override {}
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] Renderer::Common::IShader *GetStandardUnlitShader() const override { return nullptr; }
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }
    };

    // Draws once per frame with the state Scene::Render hands it, tagged with its id
    class QueuedRenderable : public IRenderable
    {
    public:
        explicit QueuedRenderable(GameObject &gameObject) : IRenderable(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "RenderQueueTest_QueuedRenderable"; }

        void Render(Renderer::Common::IRenderer *renderer) override
        {
            RenderInQueue(renderer, RenderState::Opaque());
        }
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const RenderState &state) override
        {
            float model[16]{};
            model[0] = static_cast<float>(id);
            renderer->DrawMesh(nullptr, model, nullptr, state);
        }
        void InitializeRenderResources(Renderer::Common::IRenderer *) override {}
        void CleanupRenderResources(Renderer::Common::IRenderer *) override {}

        [[nodiscard]] RenderQueueKey GetRenderQueue() const override { return key; }

        int id = 0;
        RenderQueueKey key{};
    };

    // Implements only Render, as every renderable written before the render queue does
    class LegacyRenderable final : public IRenderable
    {
    public:
        explicit LegacyRenderable(GameObject &gameObject) : IRenderable(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "RenderQueueTest_LegacyRenderable"; }

        void Render(Renderer::Common::IRenderer *renderer) override
        {
            float model[16]{};
            model[0] = static_cast<float>(id);
            renderer->DrawMesh(nullptr, model, nullptr);
        }
        void InitializeRenderResources(Renderer::Common::IRenderer *) override {}
        void CleanupRenderResources(Renderer::Common::IRenderer *) override {}

        int id = 0;
    };

    constexpr RenderQueueKey Transparent(const int sortKey = 0)
    {
        return RenderQueueKey{RenderQueue::Transparent, sortKey};
    }

    QueuedRenderable *AddRenderable(GameObject &gameObject, const int id, const RenderQueueKey key = {})
    {
        auto *renderable = gameObject.AddComponent<QueuedRenderable>();
        renderable->id = id;
        renderable->key = key;
        return renderable;
    }

    // An object with a world position, and one renderable on it
    GameObject::Ptr PlacedObject(const std::string &name, const Math::Vector3 &position, const int id,
                                 const RenderQueueKey key = {})
    {
        auto gameObject = GameObject::Create(name);
        gameObject->CreatePositionable();
        gameObject->GetPositionable()->SetPosition(position);
        AddRenderable(*gameObject, id, key);
        return gameObject;
    }

    // The default camera sits at the origin looking down -Z, so an object at z = -d is at depth d
    std::vector<RecordedDraw> RenderOnce(Scene &scene, const Camera &camera = Camera{})
    {
        RecordingRenderer renderer;
        scene.Render(&renderer, camera);
        return renderer.draws;
    }

    std::vector<int> RenderIds(Scene &scene, const Camera &camera = Camera{})
    {
        std::vector<int> ids;
        for (const RecordedDraw &draw : RenderOnce(scene, camera))
        {
            ids.push_back(draw.id);
        }
        return ids;
    }
}

TEST(RenderQueueDefaultsTest, DefaultsAreOpaqueAndUnblended)
{
    constexpr RenderQueueKey key{};
    EXPECT_EQ(key.queue, RenderQueue::Opaque);
    EXPECT_EQ(key.sortKey, 0);

    constexpr RenderState state{};
    EXPECT_TRUE(state.depthTest);
    EXPECT_TRUE(state.depthWrite);
    EXPECT_EQ(state.cull, CullMode::Back);
    EXPECT_FALSE(state.blend) << "opaque draws don't blend (#3 P2); only the Transparent queue does";
    EXPECT_EQ(RenderState::Opaque(), state);

    constexpr RenderState transparent = RenderState::Transparent();
    EXPECT_TRUE(transparent.depthTest);
    EXPECT_FALSE(transparent.depthWrite);
    EXPECT_EQ(transparent.cull, CullMode::Back);
    EXPECT_TRUE(transparent.blend);
}

TEST(RenderQueueTest, OpaqueDrawsInHierarchyOrder)
{
    const auto scene = Scene::Create("RenderQueue_OpaqueOrder");

    // Depth first, parents before children, components in the order they were added
    const auto a = GameObject::Create("A");
    const auto a1 = GameObject::Create("A1");
    const auto a1a = GameObject::Create("A1a");
    const auto a2 = GameObject::Create("A2");
    const auto b = GameObject::Create("B");
    AddRenderable(*a, 1);
    AddRenderable(*a, 2);
    AddRenderable(*a1, 3);
    AddRenderable(*a1a, 4);
    AddRenderable(*a2, 5);
    AddRenderable(*b, 6);
    a1->AddChild(a1a);
    a->AddChild(a1);
    a->AddChild(a2);
    scene->AddRootGameObjects({a, b});

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{1, 2, 3, 4, 5, 6}));
}

TEST(RenderQueueTest, OpaqueIgnoresSortKeyAndDepth)
{
    const auto scene = Scene::Create("RenderQueue_OpaqueIgnoresKeys");
    scene->AddRootGameObject(PlacedObject("Near", {0, 0, -1}, 1, RenderQueueKey{RenderQueue::Opaque, 5}));
    scene->AddRootGameObject(PlacedObject("Far", {0, 0, -9}, 2, RenderQueueKey{RenderQueue::Opaque, -5}));

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{1, 2}));
}

TEST(RenderQueueTest, TransparentDrawsAfterOpaqueBackToFront)
{
    const auto scene = Scene::Create("RenderQueue_BackToFront");
    scene->AddRootGameObject(PlacedObject("GlassNear", {0, 0, -2}, 10, Transparent()));
    scene->AddRootGameObject(PlacedObject("Wall", {0, 0, -20}, 1));
    scene->AddRootGameObject(PlacedObject("GlassFar", {0, 0, -10}, 11, Transparent()));
    scene->AddRootGameObject(PlacedObject("Floor", {0, 0, -1}, 2));
    scene->AddRootGameObject(PlacedObject("GlassMid", {1, 3, -5}, 12, Transparent()));

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{1, 2, 11, 12, 10}));
}

TEST(RenderQueueTest, DepthIsMeasuredFromTheCamera)
{
    const auto scene = Scene::Create("RenderQueue_CameraDepth");
    scene->AddRootGameObject(PlacedObject("Origin", {0, 0, 0}, 1, Transparent()));
    scene->AddRootGameObject(PlacedObject("Closer", {0, 0, 8}, 2, Transparent()));
    scene->AddRootGameObject(PlacedObject("Behind", {0, 0, 12}, 3, Transparent()));
    scene->AddRootGameObject(PlacedObject("Mid", {0, 0, 5}, 4, Transparent()));

    // Camera at z = 10 looking down -Z: depths are 10, 2, -2 and 5
    Camera camera;
    camera.SetPosition({0, 0, 10});

    EXPECT_EQ(RenderIds(*scene, camera), (std::vector<int>{1, 4, 2, 3}));
}

TEST(RenderQueueTest, DepthFollowsTheCameraRotation)
{
    // Turned 180 degrees about Y, the camera looks down +Z: depth is +z, so the object at z = 9 is the
    // farthest. Ignoring the camera (depth -z) would draw them in the opposite order.
    {
        const auto scene = Scene::Create("RenderQueue_CameraTurnedAround");
        scene->AddRootGameObject(PlacedObject("Near", {0, 0, 2}, 2, Transparent()));
        scene->AddRootGameObject(PlacedObject("Far", {0, 0, 9}, 9, Transparent()));
        scene->AddRootGameObject(PlacedObject("Mid", {0, 0, 5}, 5, Transparent()));

        Camera camera;
        camera.SetRotation(Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float>));

        EXPECT_EQ(RenderIds(*scene, camera), (std::vector<int>{9, 5, 2}));
    }

    // Turned +90 degrees about Y, the rotation is R = [[0,0,1],[0,1,0],[-1,0,0]] and the camera looks
    // down -X. Placed at x = 1, its view matrix is R^T * Translation(-1,0,0), whose row 2 is
    // (1, 0, 0, -1): depth = -(x - 1) = 1 - x, giving Near 2, Mid 5, Far 9, so the order is Far, Mid,
    // Near. The z offsets don't change that depth. This order fails if:
    // - the camera is ignored (depth = -z: 9, 0, 3, giving Near, Mid, Far);
    // - column 2 of the view matrix is used instead of row 2 (it is (-1, 0, 0, 0) here, so depth = x:
    //   -1, -8, -4, giving Near, Mid, Far as well), since R is not symmetric.
    {
        const auto scene = Scene::Create("RenderQueue_CameraTurnedSideways");
        scene->AddRootGameObject(PlacedObject("Near", {-1, 0, -9}, 1, Transparent()));
        scene->AddRootGameObject(PlacedObject("Far", {-8, 0, 0}, 2, Transparent()));
        scene->AddRootGameObject(PlacedObject("Mid", {-4, 0, -3}, 3, Transparent()));

        Camera camera;
        camera.SetPosition({1, 0, 0});
        camera.SetRotation(Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 2.0f));

        EXPECT_EQ(RenderIds(*scene, camera), (std::vector<int>{2, 3, 1}));
    }
}

TEST(RenderQueueTest, SortKeyTakesPrecedenceOverDepth)
{
    const auto scene = Scene::Create("RenderQueue_SortKey");
    scene->AddRootGameObject(PlacedObject("FarDefault", {0, 0, -10}, 1, Transparent(0)));
    scene->AddRootGameObject(PlacedObject("NearFirst", {0, 0, -1}, 2, Transparent(-1)));
    scene->AddRootGameObject(PlacedObject("FarLast", {0, 0, -50}, 3, Transparent(1)));
    scene->AddRootGameObject(PlacedObject("NearDefault", {0, 0, -3}, 4, Transparent(0)));

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{2, 1, 4, 3}));
}

TEST(RenderQueueTest, EqualDepthKeepsHierarchyOrder)
{
    const auto scene = Scene::Create("RenderQueue_Stable");
    scene->AddRootGameObject(PlacedObject("First", {-1, 0, -4}, 1, Transparent()));
    scene->AddRootGameObject(PlacedObject("Second", {1, 0, -4}, 2, Transparent()));
    scene->AddRootGameObject(PlacedObject("Third", {0, 2, -4}, 3, Transparent()));

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{1, 2, 3}));
}

TEST(RenderQueueTest, NoPositionableSortsAtDepthZero)
{
    const auto scene = Scene::Create("RenderQueue_NoPositionable");
    const auto unplaced = GameObject::Create("Unplaced");
    AddRenderable(*unplaced, 1, Transparent());
    scene->AddRootGameObject(unplaced);
    scene->AddRootGameObject(PlacedObject("BehindCamera", {0, 0, 3}, 2, Transparent()));
    scene->AddRootGameObject(PlacedObject("InFront", {0, 0, -5}, 3, Transparent()));

    // Depths 0, -3 and 5
    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{3, 1, 2}));
}

TEST(RenderQueueTest, InactiveObjectsAndComponentsAreSkipped)
{
    const auto scene = Scene::Create("RenderQueue_Inactive");

    const auto inactiveRoot = PlacedObject("InactiveRoot", {0, 0, -5}, 1, Transparent());
    const auto childOfInactive = PlacedObject("ChildOfInactive", {0, 0, -6}, 2);
    inactiveRoot->AddChild(childOfInactive);
    inactiveRoot->SetActive(false);

    const auto disabledComponent = PlacedObject("DisabledComponent", {0, 0, -7}, 3, Transparent());
    disabledComponent->GetComponent<QueuedRenderable>()->SetActive(false);
    AddRenderable(*disabledComponent, 4)->SetActive(false);

    const auto visible = PlacedObject("Visible", {0, 0, -8}, 5);
    const auto visibleGlass = PlacedObject("VisibleGlass", {0, 0, -9}, 6, Transparent());

    scene->AddRootGameObjects({inactiveRoot, disabledComponent, visible, visibleGlass});

    EXPECT_EQ(RenderIds(*scene), (std::vector<int>{5, 6}));
}

TEST(RenderQueueTest, EachQueuePassesItsRenderState)
{
    const auto scene = Scene::Create("RenderQueue_State");
    scene->AddRootGameObject(PlacedObject("Glass", {0, 0, -2}, 1, Transparent()));
    scene->AddRootGameObject(PlacedObject("Wall", {0, 0, -3}, 2));

    const std::vector<RecordedDraw> draws = RenderOnce(*scene);
    ASSERT_EQ(draws.size(), 2u);

    EXPECT_EQ(draws[0].id, 2);
    EXPECT_EQ(draws[0].state, RenderState::Opaque());
    EXPECT_TRUE(draws[0].state.depthWrite);
    EXPECT_FALSE(draws[0].state.blend);

    EXPECT_EQ(draws[1].id, 1);
    EXPECT_EQ(draws[1].state, RenderState::Transparent());
    EXPECT_TRUE(draws[1].state.depthTest);
    EXPECT_FALSE(draws[1].state.depthWrite);
    EXPECT_TRUE(draws[1].state.blend);
}

TEST(RenderQueueTest, RenderOnlyRenderableDrawsWithDefaultState)
{
    const auto scene = Scene::Create("RenderQueue_Legacy");
    const auto gameObject = GameObject::Create("Legacy");
    gameObject->AddComponent<LegacyRenderable>()->id = 7;
    scene->AddRootGameObject(gameObject);

    const std::vector<RecordedDraw> draws = RenderOnce(*scene);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0].id, 7);
    EXPECT_EQ(draws[0].state, RenderState{});
}

TEST(RenderQueueTest, NullRendererDrawsNothing)
{
    const auto scene = Scene::Create("RenderQueue_NullRenderer");
    scene->AddRootGameObject(PlacedObject("Wall", {0, 0, -3}, 1));

    EXPECT_NO_THROW(scene->Render(nullptr, Camera{}));
}
