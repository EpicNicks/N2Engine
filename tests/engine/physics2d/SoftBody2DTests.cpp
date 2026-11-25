// The 2D soft body (issue #445): SoftBody2D's shapes, its life in the 2D world (falling, resting, following and being
// moved, pinning, disabling), that queries do not see it, rollback save and restore, drawing and serialization.
// docs/physics2d.html#soft-bodies. The simulation tests are compiled only with Box2D.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DQueries.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/physics2d/SoftBody2D.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;
using namespace N2Engine::Physics2D;
using Math::Vector2;
using Math::Vector3;

namespace
{
    float TwiceArea(const std::vector<Vector2> &points)
    {
        float twice = 0.0f;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const Vector2 &a = points[i];
            const Vector2 &b = points[(i + 1) % points.size()];
            twice += a.x * b.y - b.x * a.y;
        }
        return twice;
    }

    class SoftBody2DTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        void SetUp() override
        {
            Layers::ResetToDefaults();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
            SceneManager::AddScene(Scene::Create(std::string("SoftBody2D_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
        }

        void TearDown() override
        {
            SceneManager::AddScene(Scene::Create("SoftBody2D_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
        }

        void Step(const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr Object(const std::string &name, const float x, const float y)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Vector3(x, y, 0.0f));
            _scene->AddRootGameObject(go);
            return go;
        }

        SoftBody2D *Soft(const std::string &name, const float x, const float y)
        {
            return Object(name, x, y)->AddComponent<SoftBody2D>();
        }

        /// A static box whose top is at y = 0
        void Floor()
        {
            Object("Floor", 0.0f, -1.0f)->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(50.0f, 1.0f));
        }

        static Vector2 At(const Component &component)
        {
            const Vector3 p = component.GetGameObject().GetPositionable()->GetPosition();
            return Vector2(p.x, p.y);
        }
    };
}

// ============================================================================
// Shapes and settings (no simulation)
// ============================================================================

TEST_F(SoftBody2DTest, TheShapesAreCounterClockwiseOutlines)
{
    SoftBody2D *body = Soft("Shapes", 0.0f, 0.0f);
    body->SetCircle(2.0f, 12);
    std::vector<Vector2> points = body->RestOutline();
    ASSERT_EQ(points.size(), 12u);
    for (const Vector2 &point : points)
    {
        EXPECT_NEAR(point.Length(), 2.0f, 1e-4f);
    }
    EXPECT_GT(TwiceArea(points), 0.0f);

    body->SetBox(Vector2(4.0f, 1.0f), 10);
    points = body->RestOutline();
    ASSERT_EQ(points.size(), 10u);
    EXPECT_NEAR(TwiceArea(points), 8.0f, 1e-3f) << "the corners are among the nodes, so the area is the box's";
    const auto along = std::ranges::count_if(points, [](const Vector2 &p) { return std::abs(std::abs(p.y) - 0.5f) < 1e-4f; });
    EXPECT_GT(along, 6) << "more nodes along the long sides";
    body->SetBox(Vector2(1.0f, 1.0f), 3);
    EXPECT_EQ(body->RestOutline().size(), 4u);

    // Clockwise points are turned around
    EXPECT_TRUE(body->SetOutline({Vector2(-1.0f, -1.0f), Vector2(0.0f, 1.0f), Vector2(1.0f, -1.0f)}));
    EXPECT_GT(TwiceArea(body->RestOutline()), 0.0f);
    EXPECT_EQ(body->GetNodeCount(), 3u);

    EXPECT_FALSE(body->SetOutline({Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f)}));
    EXPECT_FALSE(body->SetOutline({Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(2.0f, 0.0f)})) << "no area";
    EXPECT_FALSE(body->SetOutline(std::vector<Vector2>(65, Vector2(1.0f, 1.0f))));
    EXPECT_EQ(body->GetNodeCount(), 3u) << "a refused outline changes nothing";
    body->SetCircle(-1.0f, 8);
    EXPECT_EQ(body->GetShape(), SoftBodyShape2D::Outline);
    body->SetCircle(1.0f, 500);
    EXPECT_EQ(body->GetNodeCount(), 64u);
}

TEST_F(SoftBody2DTest, ItIsSaved)
{
    SoftBody2D *body = Soft("Saved", 0.0f, 0.0f);
    body->SetOutline({Vector2(-1.0f, -1.0f), Vector2(1.0f, -1.0f), Vector2(0.0f, 1.5f)});
    body->SetStiffness(12.0f, 0.7f, 5.0f, 0.2f);
    body->SetNodeRadius(0.25f);
    body->SetDensity(3.0f);
    body->SetNodePinned(2, true);
    body->SetColor(Common::Color{1.0f, 0.5f, 0.0f, 1.0f});
    body->SetSortingOrder(7);
    const nlohmann::json saved = body->Serialize();

    SoftBody2D *loaded = Soft("Loaded", 0.0f, 0.0f);
    loaded->Deserialize(saved);
    EXPECT_EQ(loaded->GetShape(), SoftBodyShape2D::Outline);
    EXPECT_EQ(loaded->GetNodeCount(), 3u);
    EXPECT_FLOAT_EQ(loaded->GetEdgeFrequency(), 12.0f);
    EXPECT_FLOAT_EQ(loaded->GetShapeDamping(), 0.2f);
    EXPECT_FLOAT_EQ(loaded->GetNodeRadius(), 0.25f);
    EXPECT_FLOAT_EQ(loaded->GetDensity(), 3.0f);
    EXPECT_TRUE(loaded->IsNodePinned(2));
    EXPECT_FALSE(loaded->IsNodePinned(0));
    EXPECT_EQ(loaded->GetSortingOrder(), 7);
    EXPECT_FLOAT_EQ(loaded->GetColor().g, 0.5f);
}

TEST_F(SoftBody2DTest, TheMeshCoversTheOutline)
{
    SoftBody2D *body = Soft("Mesh", 0.0f, 0.0f);
    body->SetCircle(1.0f, 8);
    body->SetNodeRadius(0.1f);
    const Renderer::Common::MeshData round = body->BuildMesh(body->RestOutline(), Vector2(0.0f, 0.0f), 2.0f);
    ASSERT_EQ(round.vertices.size(), 9u) << "the centre and the nodes";
    EXPECT_EQ(round.indices.size(), 24u);
    EXPECT_FLOAT_EQ(round.vertices[0].position[2], 2.0f);
    EXPECT_NEAR(round.vertices[0].texCoord[0], 0.5f, 1e-4f);
    const float x = round.vertices[1].position[0];
    const float y = round.vertices[1].position[1];
    EXPECT_NEAR(std::sqrt(x * x + y * y), 1.1f, 1e-4f) << "pushed out by the node radius";

    // A concave outline (an arrow head) is triangulated, not fanned across its notch
    ASSERT_TRUE(body->SetOutline({Vector2(-1.0f, -1.0f), Vector2(0.0f, 0.0f), Vector2(1.0f, -1.0f), Vector2(0.0f, 1.0f)}));
    const Renderer::Common::MeshData arrow = body->BuildMesh(body->RestOutline(), Vector2(0.0f, 0.3f), 0.0f);
    EXPECT_EQ(arrow.vertices.size(), 4u);
    EXPECT_EQ(arrow.indices.size(), 6u);

    EXPECT_TRUE(body->BuildMesh({Vector2(0.0f, 0.0f)}, Vector2(0.0f, 0.0f), 0.0f).vertices.empty())
        << "positions that are not the outline's";
}

TEST_F(SoftBody2DTest, BeforeItHasABodyItIsItsRestShapeAtTheObject)
{
    SoftBody2D *body = Soft("Rest", 3.0f, 4.0f);
    body->GetGameObject().GetPositionable()->SetLocalScale(Vector3(2.0f, 1.0f, 1.0f));
    body->SetCircle(1.0f, 4);
    EXPECT_FALSE(body->HasBody());
    EXPECT_NEAR(body->GetCentre().x, 3.0f, 1e-4f);
    const std::vector<Vector2> nodes = body->GetNodePositions();
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_NEAR(nodes[0].x, 5.0f, 1e-4f) << "scaled";
    EXPECT_NEAR(nodes[1].y, 5.0f, 1e-4f);
}

TEST_F(SoftBody2DTest, TheSoftwareRendererDrawsItFilled)
{
    constexpr int size = 64;
    SoftBody2D *body = Soft("Drawn", 0.0f, 0.0f);
    body->SetCircle(1.0f, 24);
    body->SetColor(Common::Color::Red);

    Renderer::Software::SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, size, size));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4);
    Camera camera;
    camera.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
    camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
    renderer.BeginFrame();
    renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
    _scene->Render(&renderer, camera);
    renderer.EndFrame();
    renderer.Present();
    renderer.ReadFramebuffer(rgba.data(), size, size);
    body->CleanupRenderResources(&renderer);
    renderer.Shutdown();

    const auto red = [&rgba](const int x, const int y) { return rgba[(static_cast<std::size_t>(y) * size + x) * 4]; };
    EXPECT_GT(red(32, 32), 200) << "the middle";
    EXPECT_GT(red(45, 32), 200) << "inside the circle";
    EXPECT_LT(red(60, 32), 50) << "outside it";
    EXPECT_LT(red(3, 3), 50);
}

#ifdef N2ENGINE_BOX2D_ENABLED

// ============================================================================
// Simulation
// ============================================================================

TEST_F(SoftBody2DTest, ItIsMadeInTheFirstStepFallsAndRestsOnAFloorKeepingItsShape)
{
    Floor();
    SoftBody2D *body = Soft("Blob", 0.0f, 3.0f);
    body->SetCircle(1.0f, 16);
    EXPECT_FALSE(body->HasBody()) << "made in the first fixed step";
    Step(1);
    ASSERT_TRUE(body->HasBody());
    Step(240);
    const Vector2 centre = body->GetCentre();
    EXPECT_NEAR(At(*body).x, centre.x, 1e-3f) << "the object follows the centre";
    EXPECT_NEAR(At(*body).y, centre.y, 1e-3f);
    EXPECT_GT(centre.y, 0.5f) << "it did not fall through";
    EXPECT_LT(centre.y, 1.5f) << "it came down";
    float lowest = 100.0f;
    for (const Vector2 &node : body->GetNodePositions())
    {
        lowest = std::min(lowest, node.y);
        const float distance = (node - centre).Length();
        EXPECT_GT(distance, 0.4f);
        EXPECT_LT(distance, 1.6f);
    }
    EXPECT_NEAR(lowest, 0.1f, 0.15f) << "resting on its nodes";
}

TEST_F(SoftBody2DTest, ARigidBodyRestsOnIt)
{
    Floor();
    SoftBody2D *body = Soft("Cushion", 0.0f, 1.1f);
    body->SetCircle(1.0f, 20);
    body->SetStiffness(15.0f, 0.8f, 8.0f, 0.8f);
    Step(60);
    const auto crate = Object("Crate", 0.0f, 3.0f);
    crate->AddComponent<Rigidbody2D>();
    crate->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.4f, 0.2f));
    Step(180);
    EXPECT_GT(crate->GetPositionable()->GetPosition().y, 0.6f) << "held up by the soft body, not on the floor (0.2)";
}

TEST_F(SoftBody2DTest, MovingTheObjectMovesTheWholeBody)
{
    SoftBody2D *body = Soft("Moved", 0.0f, 0.0f);
    body->SetGravityScale(0.0f);
    Step(2);
    const std::vector<Vector2> before = body->GetNodePositions();
    body->GetGameObject().GetPositionable()->SetPosition(Vector3(10.0f, -5.0f, 0.0f));
    Step(1);
    const std::vector<Vector2> after = body->GetNodePositions();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        EXPECT_NEAR(after[i].x, before[i].x + 10.0f, 0.02f);
        EXPECT_NEAR(after[i].y, before[i].y - 5.0f, 0.02f);
    }
    EXPECT_NEAR(body->GetCentre().x, 10.0f, 0.02f);
}

TEST_F(SoftBody2DTest, APinnedNodeStaysAndHoldsTheRest)
{
    SoftBody2D *body = Soft("Hung", 0.0f, 0.0f);
    body->SetCircle(1.0f, 8);
    body->SetNodePinned(2, true); // the top node
    Step(1);
    const Vector2 held = body->GetNodePositions()[2];
    Step(120);
    EXPECT_NEAR(body->GetNodePositions()[2].x, held.x, 1e-4f);
    EXPECT_NEAR(body->GetNodePositions()[2].y, held.y, 1e-4f);
    EXPECT_GT(body->GetCentre().y, -3.0f) << "it hangs";
    body->SetNodePinned(2, false);
    Step(60);
    EXPECT_LT(body->GetNodePositions()[2].y, held.y - 1.0f) << "let go, it falls";
}

TEST_F(SoftBody2DTest, AnImpulsePushesTheWholeBody)
{
    SoftBody2D *body = Soft("Pushed", 0.0f, 0.0f);
    body->SetGravityScale(0.0f);
    body->AddImpulse(Vector2(100.0f, 0.0f));
    Step(1);
    EXPECT_NEAR(body->GetVelocity().x, 0.0f, 1e-4f) << "nothing before there is a body";
    body->AddImpulse(Vector2(2.0f, 0.0f));
    Step(30);
    EXPECT_GT(body->GetCentre().x, 0.1f);
}

TEST_F(SoftBody2DTest, DisablingHoldsItWhereItIsAndDestroyingRemovesIt)
{
    SoftBody2D *body = Soft("Off", 0.0f, 10.0f);
    Step(30);
    const float height = body->GetCentre().y;
    EXPECT_LT(height, 10.0f);
    body->SetActive(false);
    Step(30);
    EXPECT_FLOAT_EQ(body->GetCentre().y, height);
    body->SetActive(true);
    Step(30);
    EXPECT_LT(body->GetCentre().y, height) << "falling again, from where it was";

    body->GetGameObject().SetActive(false);
    const float low = body->GetCentre().y;
    Step(10);
    EXPECT_FLOAT_EQ(body->GetCentre().y, low);
    body->GetGameObject().SetActive(true);

    Physics2DWorld *world = _scene->GetPhysics2DWorld();
    ASSERT_NE(world, nullptr);
    const Physics::PhysicsBodyHandle handle = body->GetHandle();
    body->GetGameObject().Destroy();
    Step(1);
    std::vector<Vector2> ring;
    Vector2 centre;
    EXPECT_FALSE(world->ReadSoftBody(handle, ring, centre));
}

TEST_F(SoftBody2DTest, AChangedShapeRebuildsItWhereItIs)
{
    SoftBody2D *body = Soft("Rebuilt", 0.0f, 5.0f);
    Step(20);
    const Vector2 at = body->GetCentre();
    body->SetCircle(0.5f, 6);
    EXPECT_FALSE(body->HasBody());
    Step(1);
    ASSERT_TRUE(body->HasBody());
    EXPECT_EQ(body->GetNodePositions().size(), 6u);
    EXPECT_NEAR(body->GetCentre().y, at.y, 0.1f);
}

TEST_F(SoftBody2DTest, QueriesDoNotSeeIt)
{
    SoftBody2D *body = Soft("Unseen", 0.0f, 0.0f);
    body->SetGravityScale(0.0f);
    Object("Wall", 5.0f, 0.0f)->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.5f, 3.0f));
    Step(2);
    ASSERT_TRUE(body->HasBody());
    RaycastHit2D hit;
    ASSERT_TRUE(Physics2D::Raycast(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), hit, 20.0f));
    EXPECT_NEAR(hit.point.x, 4.5f, 0.01f) << "the wall behind it";
    std::vector<RaycastHit2D> all;
    EXPECT_EQ(Physics2D::RaycastAll(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), all, 20.0f), 1);
    std::vector<OverlapHit2D> overlaps;
    EXPECT_EQ(Physics2D::OverlapCircle(Vector2(0.0f, 0.0f), 2.0f, overlaps), 0);
}

TEST_F(SoftBody2DTest, ARestoredStateRunsTheSameAgain)
{
    Floor();
    SoftBody2D *body = Soft("Replayed", 0.0f, 2.0f);
    body->SetCircle(1.0f, 12);
    Step(20);
    Physics2DWorld *world = _scene->GetPhysics2DWorld();
    ASSERT_NE(world, nullptr);
    std::vector<std::byte> saved;
    ASSERT_TRUE(world->SaveState(saved));
    const std::uint64_t atSave = world->HashSimulatedState();
    Step(60);
    const std::uint64_t first = world->HashSimulatedState();
    const std::vector<Vector2> nodes = body->GetNodePositions();
    EXPECT_NE(first, atSave);

    ASSERT_TRUE(world->RestoreState(saved));
    EXPECT_EQ(world->HashSimulatedState(), atSave);
    EXPECT_TRUE(body->HasBody()) << "the same body again";
    Step(60);
    EXPECT_EQ(world->HashSimulatedState(), first);
    const std::vector<Vector2> again = body->GetNodePositions();
    ASSERT_EQ(again.size(), nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        EXPECT_EQ(again[i].x, nodes[i].x);
        EXPECT_EQ(again[i].y, nodes[i].y);
    }
}

TEST_F(SoftBody2DTest, TheHashSeesItsNodes)
{
    SoftBody2D *body = Soft("Hashed", 0.0f, 0.0f);
    body->SetGravityScale(0.0f);
    Step(2);
    Physics2DWorld *world = _scene->GetPhysics2DWorld();
    const std::uint64_t still = world->HashSimulatedState();
    body->SetNodePinned(0, true);
    body->AddImpulse(Vector2(1.0f, 0.0f));
    Step(1);
    EXPECT_NE(world->HashSimulatedState(), still);
}

TEST_F(SoftBody2DTest, ARestoreToBeforeItWasMadeRebuildsIt)
{
    Floor();
    Step(2);
    Physics2DWorld *world = _scene->GetPhysics2DWorld();
    ASSERT_NE(world, nullptr);
    std::vector<std::byte> saved;
    ASSERT_TRUE(world->SaveState(saved));
    SoftBody2D *body = Soft("Late", 0.0f, 3.0f);
    Step(10);
    ASSERT_TRUE(body->HasBody());
    ASSERT_TRUE(world->RestoreState(saved));
    EXPECT_FALSE(body->HasBody()) << "it did not exist then";
    Step(1);
    EXPECT_TRUE(body->HasBody()) << "made again";
}

TEST_F(SoftBody2DTest, ARestoreDoesNotBringBackADestroyedOne)
{
    SoftBody2D *body = Soft("Gone", 0.0f, 3.0f);
    Step(5);
    Physics2DWorld *world = _scene->GetPhysics2DWorld();
    std::vector<std::byte> saved;
    ASSERT_TRUE(world->SaveState(saved));
    const Physics::PhysicsBodyHandle handle = body->GetHandle();
    body->GetGameObject().Destroy();
    Step(1);
    ASSERT_TRUE(world->RestoreState(saved));
    std::vector<Vector2> ring;
    Vector2 centre;
    EXPECT_FALSE(world->ReadSoftBody(handle, ring, centre));
    Step(5); // nothing left over to trip on
}

#endif
