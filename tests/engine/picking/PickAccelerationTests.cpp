#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/picking/ScenePicking.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// The acceleration of picking (#101): the mesh BVH and the shared canvas layout. Each must answer exactly as the
// plain path does (RaycastMeshLinear, the layout made per call), also after the scene or the mesh changed.

using namespace N2Engine;
using Math::Quaternion;
using Math::Ray;
using Math::Vector2;
using Math::Vector3;

namespace
{
    using Matrix4 = Math::Matrix<float, 4, 4>;

    /// A small deterministic generator, so a failure repeats
    class Lcg
    {
    public:
        explicit Lcg(const std::uint32_t seed) : _state(seed) {}

        /// In [lo, hi)
        float Next(const float lo, const float hi)
        {
            _state = _state * 1664525u + 1013904223u;
            const float unit = static_cast<float>((_state >> 8) & 0xFFFFFFu) / static_cast<float>(0x1000000);
            return lo + unit * (hi - lo);
        }

        Vector3 NextPoint(const float lo, const float hi) { return Vector3(Next(lo, hi), Next(lo, hi), Next(lo, hi)); }

    private:
        std::uint32_t _state;
    };

    Renderer::Common::Vertex MakeVertex(const Vector3 &p)
    {
        Renderer::Common::Vertex vertex{};
        vertex.position[0] = p.x;
        vertex.position[1] = p.y;
        vertex.position[2] = p.z;
        return vertex;
    }

    /// `count` small random triangles scattered in a box of half-size `spread`, in two submeshes
    std::shared_ptr<Rendering::Mesh> MakeSoup(const std::uint32_t seed, const std::size_t count, const float spread,
                                              const Vector3 &offset = Vector3(0.0f, 0.0f, 0.0f))
    {
        Lcg random(seed);
        Renderer::Common::MeshData data;
        for (std::size_t i = 0; i < count; ++i)
        {
            const Vector3 center = offset + random.NextPoint(-spread, spread);
            for (int corner = 0; corner < 3; ++corner)
            {
                data.vertices.push_back(MakeVertex(center + random.NextPoint(-0.3f, 0.3f)));
                data.indices.push_back(static_cast<std::uint32_t>(data.vertices.size() - 1));
            }
        }
        const auto firstHalf = static_cast<std::uint32_t>((count / 2) * 3);
        const auto total = static_cast<std::uint32_t>(count * 3);
        std::vector<Rendering::Submesh> submeshes;
        submeshes.push_back(Rendering::Submesh{0, firstHalf, {}});
        submeshes.push_back(Rendering::Submesh{firstHalf, total - firstHalf, {}});
        return Rendering::Mesh::Create(std::move(data), std::move(submeshes));
    }

    /// One triangle at depth z, for the invalidation tests
    Renderer::Common::MeshData MakeTriangleAt(const float z)
    {
        Renderer::Common::MeshData data;
        data.vertices = {MakeVertex(Vector3(-1.0f, -1.0f, z)), MakeVertex(Vector3(1.0f, -1.0f, z)),
                         MakeVertex(Vector3(0.0f, 1.0f, z))};
        data.indices = {0, 1, 2};
        return data;
    }

    Matrix4 ModelOf(const Vector3 &position, const Quaternion &rotation, const Vector3 &scale)
    {
        auto object = GameObject::Create("Model");
        object->CreatePositionable();
        object->GetPositionable()->SetPosition(position);
        object->GetPositionable()->SetRotation(rotation);
        object->GetPositionable()->SetScale(scale);
        return object->GetPositionable()->GetLocalToWorldMatrix();
    }

    /// A ray from a random point on a sphere around the origin to a random point near it
    Ray RandomRay(Lcg &random, const float radius, const float aim)
    {
        Vector3 from = random.NextPoint(-1.0f, 1.0f);
        const float length = std::max(0.1f, std::sqrt(from.Dot(from)));
        from = from * (radius / length);
        const Vector3 target = random.NextPoint(-aim, aim);
        return Ray(from, target - from);
    }

    void ExpectSameAsLinear(const Rendering::Mesh &mesh, const Matrix4 &model, const std::size_t rays,
                            const std::uint32_t seed, const float aim, const char *what)
    {
        Lcg random(seed);
        std::size_t hits = 0;
        for (std::size_t i = 0; i < rays; ++i)
        {
            const Ray ray = RandomRay(random, 8.0f, aim);
            const float limit = (i % 3 == 0) ? 9.0f : std::numeric_limits<float>::infinity();
            const std::optional<float> fast = Picking::RaycastMesh(mesh, model, ray, limit);
            const std::optional<float> slow = Picking::RaycastMeshLinear(mesh, model, ray, limit);
            ASSERT_EQ(fast.has_value(), slow.has_value()) << what << " ray " << i;
            if (fast)
            {
                ++hits;
                EXPECT_EQ(*fast, *slow) << what << " ray " << i << " (bit for bit)";
            }
        }
        EXPECT_GT(hits, rays / 20) << what << ": the rays should hit often enough to mean something";
    }
}

// ==================== The mesh BVH ====================

TEST(PickBvhTest, ATriangleSoupAnswersAsTheLinearScanDoes)
{
    const auto soup = MakeSoup(7u, 1500, 3.0f);
    ASSERT_NE(soup, nullptr);
    ExpectSameAsLinear(*soup, Matrix4::identity(), 600, 11u, 3.0f, "identity");
    ExpectSameAsLinear(*soup, ModelOf(Vector3(0.5f, -0.25f, 1.0f), Quaternion::FromEulerAngles(0.4f, 1.1f, -0.7f),
                                      Vector3(1.5f, 0.6f, 2.0f)),
                       600, 12u, 3.0f, "rotated and scaled unevenly");
}

TEST(PickBvhTest, AFlatOrTinyModelAnswersAsTheLinearScanDoes)
{
    const auto soup = MakeSoup(21u, 400, 2.0f);
    ASSERT_NE(soup, nullptr);
    // Squashed flat: no inverse exists, the BVH must not need one
    ExpectSameAsLinear(*soup, ModelOf(Vector3(0.0f, 0.0f, 0.0f), Quaternion::Identity, Vector3(1.0f, 1.0f, 0.0f)), 400,
                       22u, 2.0f, "flat");
    ExpectSameAsLinear(*soup, ModelOf(Vector3(0.0f, 0.0f, 0.0f), Quaternion::Identity, Vector3(0.001f, 0.001f, 0.001f)),
                       400, 23u, 0.0015f, "tiny");
    // Zero scale: both miss everything
    const Matrix4 zero = ModelOf(Vector3(0.0f, 0.0f, 0.0f), Quaternion::Identity, Vector3(0.0f, 0.0f, 0.0f));
    Lcg random(24u);
    for (int i = 0; i < 50; ++i)
    {
        const Ray ray = RandomRay(random, 8.0f, 1.0f);
        EXPECT_EQ(Picking::RaycastMesh(*soup, zero, ray).has_value(), Picking::RaycastMeshLinear(*soup, zero, ray).has_value());
    }
}

TEST(PickBvhTest, TheBuiltInShapesAnswerAsTheLinearScanDoes)
{
    for (const Rendering::BuiltinMesh which :
         {Rendering::BuiltinMesh::Cube, Rendering::BuiltinMesh::Sphere, Rendering::BuiltinMesh::Quad})
    {
        const auto mesh = Rendering::Mesh::GetBuiltin(which);
        ExpectSameAsLinear(*mesh, ModelOf(Vector3(0.1f, 0.2f, 0.3f), Quaternion::FromEulerAngles(0.3f, 0.2f, 0.9f),
                                          Vector3(2.0f, 1.0f, 1.5f)),
                           500, 31u + static_cast<std::uint32_t>(which), 0.6f, "built-in");
    }
}

TEST(PickBvhTest, ARayAlongAnEdgeOrThroughAVertexAnswersAsTheLinearScanDoes)
{
    // The cube's corners and edges: rays aimed exactly at them, where the edge tolerance decides
    const auto cube = Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube);
    const Matrix4 identity = Matrix4::identity();
    for (const float x : {-0.5f, 0.0f, 0.5f})
    {
        for (const float y : {-0.5f, 0.0f, 0.5f})
        {
            const Ray ray(Vector3(x, y, 5.0f), Vector3(0.0f, 0.0f, -1.0f));
            const auto fast = Picking::RaycastMesh(*cube, identity, ray);
            const auto slow = Picking::RaycastMeshLinear(*cube, identity, ray);
            ASSERT_EQ(fast.has_value(), slow.has_value()) << x << ", " << y;
            if (fast)
            {
                EXPECT_EQ(*fast, *slow);
            }
        }
    }
}

TEST(PickBvhTest, ASetDataMakesTheCachedBvhStale)
{
    auto mesh = Rendering::Mesh::Create(MakeTriangleAt(0.0f));
    ASSERT_NE(mesh, nullptr);
    const Matrix4 identity = Matrix4::identity();
    const Ray ray(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f));

    const auto before = Picking::RaycastMesh(*mesh, identity, ray);
    ASSERT_TRUE(before.has_value());
    EXPECT_NEAR(*before, 5.0f, 1e-4f);
    EXPECT_TRUE(mesh->HasCpuCache()) << "the first pick built the BVH and left it with the mesh";
    EXPECT_EQ(Picking::RaycastMesh(*mesh, identity, ray), before) << "the second pick reuses it";

    const std::uint64_t version = mesh->GetVersion();
    ASSERT_TRUE(mesh->SetData(MakeTriangleAt(2.0f)));
    EXPECT_GT(mesh->GetVersion(), version);
    EXPECT_FALSE(mesh->HasCpuCache()) << "new geometry: the old BVH is not served";

    const auto after = Picking::RaycastMesh(*mesh, identity, ray);
    ASSERT_TRUE(after.has_value());
    EXPECT_NEAR(*after, 3.0f, 1e-4f) << "the triangle moved to z = 2";
    EXPECT_EQ(after, Picking::RaycastMeshLinear(*mesh, identity, ray));

    // Different shape altogether (a bigger mesh): still the linear answer
    Renderer::Common::MeshData bigger = MakeSoup(5u, 50, 2.0f)->GetMeshData();
    ASSERT_TRUE(mesh->SetData(std::move(bigger)));
    ExpectSameAsLinear(*mesh, identity, 200, 41u, 2.0f, "after SetData");
}

TEST(PickBvhTest, TwoMeshesDoNotShareABvh)
{
    auto a = Rendering::Mesh::Create(MakeTriangleAt(0.0f));
    auto b = Rendering::Mesh::Create(MakeTriangleAt(-3.0f));
    const Matrix4 identity = Matrix4::identity();
    const Ray ray(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f));
    const auto hitA = Picking::RaycastMesh(*a, identity, ray);
    const auto hitB = Picking::RaycastMesh(*b, identity, ray);
    ASSERT_TRUE(hitA.has_value());
    ASSERT_TRUE(hitB.has_value());
    EXPECT_NEAR(*hitA, 5.0f, 1e-4f);
    EXPECT_NEAR(*hitB, 8.0f, 1e-4f);
}

TEST(PickBvhTest, ATypedSlotServesOnlyTheTypeItHolds)
{
    auto mesh = Rendering::Mesh::Create(MakeTriangleAt(0.0f));
    ASSERT_NE(mesh, nullptr);
    EXPECT_FALSE(mesh->HasCpuCache());
    mesh->SetCpuCache(std::make_shared<const int>(7));
    EXPECT_TRUE(mesh->HasCpuCache());
    ASSERT_NE(mesh->GetCpuCache<int>(), nullptr);
    EXPECT_EQ(*mesh->GetCpuCache<int>(), 7);
    EXPECT_EQ(mesh->GetCpuCache<double>(), nullptr) << "another type reads as empty, never reinterpreted";
}

TEST(PickBvhTest, AMeshAuthoredFarFromTheOriginUnderACancellingTranslationAnswersAsTheLinearScanDoes)
{
    // Vertices near (1e5, 1e5, 1e5), brought back to the origin by the model: the transform adds terms of 1e5 to get
    // a result near 0, so its rounding is far larger than the result's size suggests
    const Vector3 farOrigin(100000.0f, 100000.0f, 100000.0f);
    const auto soup = MakeSoup(61u, 600, 2.0f, farOrigin);
    ASSERT_NE(soup, nullptr);
    ExpectSameAsLinear(*soup, ModelOf(Vector3(-100000.0f, -100000.0f, -100000.0f), Quaternion::Identity,
                                      Vector3(1.0f, 1.0f, 1.0f)),
                       800, 62u, 2.0f, "far and cancelled");
    // Not exactly cancelling (a rotation is left out: it would swing the soup far from the origin, off the rays)
    ExpectSameAsLinear(*soup, ModelOf(Vector3(-100000.5f, -99999.75f, -100000.25f), Quaternion::Identity,
                                      Vector3(1.0f, 1.0f, 1.0f)),
                       800, 63u, 2.0f, "far, nearly cancelled");
}

TEST(PickBvhTest, ManyIdenticalTrianglesAnswerAsTheLinearScanDoes)
{
    // Every centroid equal: the split cannot separate them by position, and must still end in leaves
    Renderer::Common::MeshData data;
    for (int i = 0; i < 300; ++i)
    {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.push_back(MakeVertex(Vector3(-1.0f, -1.0f, 0.0f)));
        data.vertices.push_back(MakeVertex(Vector3(1.0f, -1.0f, 0.0f)));
        data.vertices.push_back(MakeVertex(Vector3(0.0f, 1.0f, 0.0f)));
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2});
    }
    const auto stacked = Rendering::Mesh::Create(std::move(data));
    ASSERT_NE(stacked, nullptr);
    const Matrix4 identity = Matrix4::identity();
    const Ray ray(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f));
    const auto fast = Picking::RaycastMesh(*stacked, identity, ray);
    ASSERT_TRUE(fast.has_value());
    EXPECT_EQ(fast, Picking::RaycastMeshLinear(*stacked, identity, ray));
    EXPECT_FALSE(Picking::RaycastMesh(*stacked, identity, Ray(Vector3(3.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, -1.0f)))
                     .has_value());
}

TEST(PickBvhTest, TrianglesWithNonFiniteVerticesAreStillTestedLikeTheLinearScan)
{
    Renderer::Common::MeshData data = MakeSoup(71u, 100, 2.0f)->GetMeshData();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Triangles with a NaN vertex, not first (so the mesh box stays finite)
    for (const Vector3 &bad : {Vector3(nan, 0.0f, 0.0f), Vector3(0.0f, nan, 1.0f)})
    {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.push_back(MakeVertex(Vector3(0.0f, 0.0f, 0.0f)));
        data.vertices.push_back(MakeVertex(Vector3(1.0f, 0.0f, 0.0f)));
        data.vertices.push_back(MakeVertex(bad));
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2});
    }
    const auto mesh = Rendering::Mesh::Create(std::move(data));
    ASSERT_NE(mesh, nullptr);
    ExpectSameAsLinear(*mesh, Matrix4::identity(), 400, 72u, 2.0f, "non-finite vertices");
}

TEST(PickBvhTest, TheWalkTestsAFewTrianglesOfAMeshOfManyNotAllOfThem)
{
    constexpr std::size_t count = 20000;
    const auto soup = MakeSoup(81u, count, 20.0f);
    ASSERT_NE(soup, nullptr);
    const Matrix4 identity = Matrix4::identity();
    (void)Picking::RaycastMesh(*soup, identity, Ray(Vector3(0.0f, 0.0f, 50.0f), Vector3(0.0f, 0.0f, -1.0f))); // builds it

    Lcg random(82u);
    constexpr int rays = 60;
    Picking::GetPickStats() = {};
    for (int i = 0; i < rays; ++i)
    {
        (void)Picking::RaycastMesh(*soup, identity, RandomRay(random, 60.0f, 20.0f));
    }
    const Picking::PickStats &stats = Picking::GetPickStats();
    EXPECT_GT(stats.bvhNodesVisited, 0u);
    EXPECT_LT(stats.bvhTrianglesTested / rays, count / 20) << "a pick should test a few triangles, not scan the mesh";
    EXPECT_LT(stats.bvhNodesVisited / rays, count / 40) << "and visit a small part of the tree";
}

// ==================== PickGameObject over a changing scene ====================

namespace
{
    struct Placed
    {
        GameObject::Ptr object;
        std::shared_ptr<Rendering::Mesh> mesh;
    };

    Placed AddSoup(Scene &scene, const std::string &name, const std::shared_ptr<Rendering::Mesh> &mesh,
                   const Vector3 &position)
    {
        auto object = GameObject::Create(name);
        auto *renderer = object->AddComponent<Rendering::MeshRenderer>();
        renderer->SetMesh(mesh);
        object->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(object);
        return Placed{object, mesh};
    }

    /// What PickGameObject must answer: every candidate tested by the linear scan, nearest wins (the earlier one on a tie)
    GameObject *ReferencePick(const std::vector<Placed> &placed, const Ray &ray, float &distance)
    {
        GameObject *best = nullptr;
        distance = 0.0f;
        for (const Placed &item : placed)
        {
            if (!item.object || !item.object->IsActiveInHierarchy())
            {
                continue;
            }
            const Matrix4 model = item.object->GetPositionable()->GetLocalToWorldMatrix();
            if (const auto t = Picking::RaycastMeshLinear(*item.mesh, model, ray))
            {
                if (best == nullptr || *t < distance)
                {
                    best = item.object.get();
                    distance = *t;
                }
            }
        }
        return best;
    }

    void ExpectPicksMatchReference(const Scene &scene, const std::vector<Placed> &placed, const std::uint32_t seed,
                                   const char *what)
    {
        Lcg random(seed);
        std::size_t hits = 0;
        for (int i = 0; i < 300; ++i)
        {
            const Ray ray = RandomRay(random, 15.0f, 4.0f);
            float expectedDistance = 0.0f;
            GameObject *expected = ReferencePick(placed, ray, expectedDistance);
            const Picking::PickHit hit = Picking::PickGameObject(scene, ray);
            ASSERT_EQ(hit.gameObject, expected) << what << " ray " << i;
            if (expected != nullptr)
            {
                ++hits;
                EXPECT_EQ(hit.distance, expectedDistance) << what << " ray " << i;
            }
        }
        EXPECT_GT(hits, 10u) << what;
    }

    std::unique_ptr<Scene> EditScene(const std::string &name)
    {
        auto scene = Scene::Create(name);
        scene->SetEditMode(true);
        return scene;
    }
}

TEST(PickAccelerationTest, PicksMatchTheLinearScanAsObjectsMoveAreAddedAndRemoved)
{
    auto scene = EditScene("PickAcceleration_Scene");
    std::vector<Placed> placed;
    for (int i = 0; i < 6; ++i)
    {
        // Distinct meshes, and one shared by two objects
        const auto mesh = (i == 5) ? placed[0].mesh : MakeSoup(100u + static_cast<std::uint32_t>(i), 300, 1.0f);
        placed.push_back(AddSoup(*scene, "Soup" + std::to_string(i), mesh,
                                 Vector3(static_cast<float>(i % 3) * 2.5f - 2.5f, static_cast<float>(i / 3) * 2.5f - 1.0f, 0.0f)));
    }
    ExpectPicksMatchReference(*scene, placed, 51u, "initial");

    // Moved, rotated, scaled
    placed[2].object->GetPositionable()->SetPosition(Vector3(-1.0f, 3.0f, 1.0f));
    placed[3].object->GetPositionable()->SetRotation(Quaternion::FromEulerAngles(0.5f, 0.3f, 0.1f));
    placed[4].object->GetPositionable()->SetScale(Vector3(2.0f, 0.5f, 1.5f));
    ExpectPicksMatchReference(*scene, placed, 52u, "after moves");

    // Added
    placed.push_back(AddSoup(*scene, "Late", MakeSoup(200u, 300, 1.0f), Vector3(0.0f, 0.0f, 2.0f)));
    ExpectPicksMatchReference(*scene, placed, 53u, "after an add");

    // Switched off, then removed
    placed[1].object->SetActive(false);
    ExpectPicksMatchReference(*scene, placed, 54u, "after SetActive(false)");
    placed[1].object->SetActive(true);
    ExpectPicksMatchReference(*scene, placed, 55u, "after SetActive(true)");
    ASSERT_TRUE(scene->RemoveRootGameObject(placed[0].object));
    placed[0].object.reset();
    ExpectPicksMatchReference(*scene, placed, 56u, "after a removal");

    // The shared mesh changing under its users
    ASSERT_TRUE(placed[5].mesh->SetData(MakeSoup(300u, 200, 1.5f)->GetMeshData()));
    ExpectPicksMatchReference(*scene, placed, 57u, "after the mesh's data changed");
}

TEST(PickAccelerationTest, AMeshSharedByTwoObjectsIsRebuiltForBothWhenItsDataChanges)
{
    auto scene = EditScene("PickAcceleration_Shared");
    auto shared = Rendering::Mesh::Create(MakeTriangleAt(0.0f));
    ASSERT_NE(shared, nullptr);
    std::vector<Placed> placed;
    placed.push_back(AddSoup(*scene, "Left", shared, Vector3(-3.0f, 0.0f, 0.0f)));
    placed.push_back(AddSoup(*scene, "Right", shared, Vector3(3.0f, 0.0f, 0.0f)));
    const Ray overLeft(Vector3(-3.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));
    const Ray overRight(Vector3(3.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));
    EXPECT_EQ(Picking::PickGameObject(*scene, overLeft).gameObject, placed[0].object.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, overRight).gameObject, placed[1].object.get());

    // Moved away from both (the triangle is now at y 5 to 7): both miss, then both hit again where it is
    Renderer::Common::MeshData up = MakeTriangleAt(0.0f);
    for (auto &vertex : up.vertices)
    {
        vertex.position[1] += 6.0f;
    }
    ASSERT_TRUE(shared->SetData(std::move(up)));
    EXPECT_EQ(Picking::PickGameObject(*scene, overLeft).gameObject, nullptr);
    EXPECT_EQ(Picking::PickGameObject(*scene, overRight).gameObject, nullptr);
    const Ray overLeftNow(Vector3(-3.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));
    const Ray overRightNow(Vector3(3.0f, 6.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));
    EXPECT_EQ(Picking::PickGameObject(*scene, overLeftNow).gameObject, placed[0].object.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, overRightNow).gameObject, placed[1].object.get());
}

// ==================== The canvas layout ====================

namespace
{
    GameObject::Ptr AddPanel(const GameObject::Ptr &parent, const std::string &name, const UI::Rect &local)
    {
        auto element = UI::UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<UI::RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Vector2{local.x, local.y});
        rectTransform->SetSizeDelta(Vector2{local.width, local.height});
        element->AddComponent<UI::Image>();
        parent->AddChild(element, false);
        return element;
    }

    void ExpectSameBox(const std::optional<BoundingBox> &a, const std::optional<BoundingBox> &b, const char *what)
    {
        ASSERT_EQ(a.has_value(), b.has_value()) << what;
        if (a)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                EXPECT_EQ(a->min[axis], b->min[axis]) << what << " min " << axis;
                EXPECT_EQ(a->max[axis], b->max[axis]) << what << " max " << axis;
            }
        }
    }
}

TEST(PickLayoutCacheTest, TheCanvasBoundsFromASharedLayoutAreTheCanvasBounds)
{
    auto scene = EditScene("PickLayout_Bounds");
    auto canvas = UI::UISystem::CreateCanvas("Canvas", UI::CanvasRenderMode::WorldSpace);
    scene->AddRootGameObject(canvas);
    AddPanel(canvas, "Out", UI::Rect{150.0f, 0.0f, 50.0f, 50.0f});
    auto *component = canvas->GetComponent<UI::Canvas>();
    ExpectSameBox(component->ComputeWorldBounds(UI::UISystem::CollectWorldCanvasGraphics(*component)),
                  component->GetWorldBounds(), "world canvas");

    auto overlay = UI::UISystem::CreateCanvas("Overlay");
    scene->AddRootGameObject(overlay);
    auto *overlayCanvas = overlay->GetComponent<UI::Canvas>();
    EXPECT_FALSE(overlayCanvas->ComputeWorldBounds(UI::UISystem::CollectWorldCanvasGraphics(*overlayCanvas)).has_value());
}

TEST(PickLayoutCacheTest, EntityBoundsWithASharedCacheMatchThoseWithout)
{
    auto scene = EditScene("PickLayout_Entities");
    auto canvas = UI::UISystem::CreateCanvas("Canvas", UI::CanvasRenderMode::WorldSpace);
    canvas->GetPositionable()->SetPosition(Vector3(1.0f, 2.0f, 3.0f));
    scene->AddRootGameObject(canvas);
    std::vector<GameObject::Ptr> objects{canvas};
    for (int i = 0; i < 8; ++i)
    {
        objects.push_back(AddPanel(canvas, "Panel" + std::to_string(i),
                                   UI::Rect{static_cast<float>(i) * 10.0f, 5.0f, 8.0f, 20.0f + static_cast<float>(i)}));
    }
    auto second = UI::UISystem::CreateCanvas("Second", UI::CanvasRenderMode::WorldSpace);
    scene->AddRootGameObject(second);
    objects.push_back(second);
    objects.push_back(AddPanel(second, "OnSecond", UI::Rect{10.0f, 10.0f, 30.0f, 30.0f}));

    const auto check = [&](const char *what)
    {
        Picking::LayoutCache shared;
        for (const GameObject::Ptr &object : objects)
        {
            ExpectSameBox(Picking::GetGameObjectBounds(*object, shared), Picking::GetGameObjectBounds(*object), what);
        }
    };
    check("initial");

    // A panel moved and one switched off: a new request (a new cache) sees the new layout
    objects[3]->GetComponent<UI::RectTransform>()->SetAnchoredPosition(Vector2{60.0f, 40.0f});
    objects[5]->SetActive(false);
    check("after the layout changed");
    canvas->GetPositionable()->SetPosition(Vector3(-4.0f, 0.0f, 0.0f));
    check("after the canvas moved");
}

TEST(PickLayoutCacheTest, ACanvasIsLaidOutOncePerPickAndOncePerBoundsRequest)
{
    auto scene = EditScene("PickLayout_Counts");
    std::vector<GameObject::Ptr> objects;
    for (int c = 0; c < 2; ++c)
    {
        auto canvas = UI::UISystem::CreateCanvas("Canvas" + std::to_string(c), UI::CanvasRenderMode::WorldSpace);
        canvas->GetPositionable()->SetPosition(Vector3(static_cast<float>(c) * 3.0f, 0.0f, 0.0f));
        scene->AddRootGameObject(canvas);
        objects.push_back(canvas);
        for (int i = 0; i < 6; ++i)
        {
            objects.push_back(AddPanel(canvas, "Panel", UI::Rect{static_cast<float>(i) * 10.0f, 0.0f, 8.0f, 30.0f}));
        }
    }
    const Ray ray(Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));

    Picking::GetPickStats() = {};
    (void)Picking::PickGameObject(*scene, ray);
    EXPECT_EQ(Picking::GetPickStats().canvasLayouts, 2u) << "each of the two canvases once, not twice";

    Picking::GetPickStats() = {};
    {
        Picking::LayoutCache shared;
        for (const GameObject::Ptr &object : objects)
        {
            (void)Picking::GetGameObjectBounds(*object, shared);
        }
    }
    EXPECT_EQ(Picking::GetPickStats().canvasLayouts, 2u) << "14 objects asked, one layout per canvas";

    Picking::GetPickStats() = {};
    for (const GameObject::Ptr &object : objects)
    {
        (void)Picking::GetGameObjectBounds(*object);
    }
    EXPECT_GE(Picking::GetPickStats().canvasLayouts, objects.size()) << "without a shared cache each call lays out";
}

TEST(PickLayoutCacheTest, PicksAcrossCanvasChangesMatchAFreshLayout)
{
    auto scene = EditScene("PickLayout_Picks");
    auto canvas = UI::UISystem::CreateCanvas("Canvas", UI::CanvasRenderMode::WorldSpace);
    scene->AddRootGameObject(canvas);
    const auto left = AddPanel(canvas, "Left", UI::Rect{0.0f, 0.0f, 50.0f, 100.0f});
    const auto right = AddPanel(canvas, "Right", UI::Rect{50.0f, 0.0f, 50.0f, 100.0f});
    const Ray overLeft(Vector3(-0.25f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));
    const Ray overRight(Vector3(0.25f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f));

    EXPECT_EQ(Picking::PickGameObject(*scene, overLeft).gameObject, left.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, overRight).gameObject, right.get());

    // Each pick lays the canvas out anew: a panel that grew over the other is what the next pick sees
    left->GetComponent<UI::RectTransform>()->SetSizeDelta(Vector2{100.0f, 100.0f});
    EXPECT_EQ(Picking::PickGameObject(*scene, overLeft).gameObject, left.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, overRight).gameObject, right.get()) << "later in order, so on top";

    right->SetActive(false);
    EXPECT_EQ(Picking::PickGameObject(*scene, overRight).gameObject, left.get()) << "the one under it shows";

    canvas->GetPositionable()->SetPosition(Vector3(5.0f, 0.0f, 0.0f));
    EXPECT_EQ(Picking::PickGameObject(*scene, overLeft).gameObject, nullptr) << "the canvas moved away";
    EXPECT_EQ(Picking::PickGameObject(*scene, Ray(Vector3(5.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f))).gameObject,
              left.get());
}
