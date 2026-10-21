// The scene query conformance tests (stage J5), run as TYPED tests over every backend this build has (PhysX and
// Jolt): Raycast, RaycastAll, SphereCast, CapsuleCast and the three overlaps, against the rules of QueryRules
// (Raycast.hpp): the nearest hit with ties broken by body handle, lists sorted by distance and handle, a start inside a
// collider hit at distance 0, degenerate input finding nothing, layer masks and QueryTriggers.
//
// A difference between the backends that a test has to allow for is written next to the assertion and listed in
// docs/physics.html ("Jolt backend", differences).

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "engine/Layers.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2PhysicsTests
{
    template <typename Tag>
    class PhysicsQueryTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        // A static box collider of full size `size` at `position`
        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const float size = 1.0f)
        {
            const auto go = this->Spawn(name, position);
            go->template AddComponent<BoxCollider>()->SetSize(Vector3(size, size, size));
            return go;
        }

        // True when handle `a` comes before `b` in the order of QueryRules: index, then generation
        static bool HandleBefore(const PhysicsBodyHandle &a, const PhysicsBodyHandle &b)
        {
            return a.index != b.index ? a.index < b.index : a.generation < b.generation;
        }

        static std::set<const GameObject *> ObjectsOf(const std::vector<OverlapHit> &hits)
        {
            std::set<const GameObject *> objects;
            for (const OverlapHit &hit : hits)
            {
                objects.insert(hit.gameObject);
            }
            return objects;
        }

        static bool IsSortedByHandle(const std::vector<OverlapHit> &hits)
        {
            for (std::size_t i = 1; i < hits.size(); ++i)
            {
                if (HandleBefore(hits[i].bodyHandle, hits[i - 1].bodyHandle))
                {
                    return false;
                }
            }
            return true;
        }
    };

    TYPED_TEST_SUITE(PhysicsQueryTest, AllBackends, BackendNames);

    namespace
    {
        const Vector3 Forward(0.0f, 0.0f, 1.0f);
        const Vector3 Backward(0.0f, 0.0f, -1.0f);
        const Vector3 Right(1.0f, 0.0f, 0.0f);
        const Vector3 Down(0.0f, -1.0f, 0.0f);
        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
        constexpr float kInf = std::numeric_limits<float>::infinity();
        // The tolerance of a hit's distance and position: Jolt's and PhysX's shape casts agree to well within this
        constexpr float kHitTolerance = 5e-3f;
    }

    // ============================================================================
    // Raycast
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, RaycastHitsTheNearestColliderAndDescribesTheSurface)
    {
        const auto nearBox = this->SpawnBox("Near", Vector3(0.0f, 0.0f, 5.0f));
        this->SpawnBox("Far", Vector3(0.0f, 0.0f, 9.0f));
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_TRUE(hit.hit);
        EXPECT_EQ(hit.gameObject, nearBox.get());
        EXPECT_EQ(hit.collider, nearBox->template GetComponent<BoxCollider>());
        EXPECT_EQ(hit.rigidbody, nullptr);
        EXPECT_TRUE(hit.bodyHandle.IsValid());
        EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);
        ExpectVectorNear(hit.point, Vector3(0.0f, 0.0f, 4.5f), 1e-3f);
        ExpectVectorNear(hit.normal, Backward, 1e-3f);

        // The direction is normalised: a longer one gives the same hit
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Vector3(0.0f, 0.0f, 3.0f), hit, 100.0f));
        EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);
    }

    TYPED_TEST(PhysicsQueryTest, RaycastMissesWhatIsBesideBehindOrBeyondTheDistance)
    {
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        RaycastHit hit;
        EXPECT_FALSE(Raycast::Single(Vector3(3.0f, 0.0f, 0.0f), Forward, hit, 100.0f)) << "beside";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Backward, hit, 100.0f)) << "behind";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 4.0f)) << "too short";
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 4.6f)) << "just long enough";
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, kInf)) << "an infinite distance is clamped";
        EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);

        // A miss clears the hit it was given
        EXPECT_FALSE(Raycast::Single(Vector3(3.0f, 0.0f, 0.0f), Forward, hit, 100.0f));
        EXPECT_FALSE(hit.hit);
        EXPECT_EQ(hit.gameObject, nullptr);
        EXPECT_EQ(hit.collider, nullptr);
        EXPECT_FALSE(hit.bodyHandle.IsValid());
    }

    TYPED_TEST(PhysicsQueryTest, RaycastAllIsSortedByDistance)
    {
        // Created out of order, so the order of the result is the query's and not the scene's
        const auto farBox = this->SpawnBox("Far", Vector3(0.0f, 0.0f, 8.0f));
        const auto nearBox = this->SpawnBox("Near", Vector3(0.0f, 0.0f, 3.0f));
        const auto middleBox = this->SpawnBox("Middle", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        std::vector<RaycastHit> hits(7); // the previous content is replaced
        ASSERT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f), 3);
        ASSERT_EQ(hits.size(), 3u);
        EXPECT_EQ(hits[0].gameObject, nearBox.get());
        EXPECT_EQ(hits[1].gameObject, middleBox.get());
        EXPECT_EQ(hits[2].gameObject, farBox.get());
        EXPECT_NEAR(hits[0].distance, 2.5f, 1e-3f);
        EXPECT_NEAR(hits[1].distance, 4.5f, 1e-3f);
        EXPECT_NEAR(hits[2].distance, 7.5f, 1e-3f);

        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 6.0f), 2) << "the distance limits the list";
        EXPECT_EQ(Raycast::All(Vector3(3.0f, 0.0f, 0.0f), Forward, hits, 100.0f), 0);
        EXPECT_TRUE(hits.empty());
    }

    TYPED_TEST(PhysicsQueryTest, EqualHitsAreOrderedByBodyHandle)
    {
        // Two boxes in the same place: every ray hits both at the same distance
        this->SpawnBox("First", Vector3(0.0f, 0.0f, 5.0f));
        this->SpawnBox("Second", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        std::vector<RaycastHit> hits;
        ASSERT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f), 2);
        EXPECT_NEAR(hits[0].distance, hits[1].distance, 1e-4f);
        EXPECT_TRUE(TestFixture::HandleBefore(hits[0].bodyHandle, hits[1].bodyHandle))
            << "the lower handle comes first";

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_EQ(hit.bodyHandle, hits[0].bodyHandle) << "the single hit is the one that comes first";

        // From the other side too: a different face of the same two boxes
        ASSERT_EQ(Raycast::All(Vector3(0.0f, 0.0f, 10.0f), Backward, hits, 100.0f), 2);
        EXPECT_TRUE(TestFixture::HandleBefore(hits[0].bodyHandle, hits[1].bodyHandle));
        ASSERT_TRUE(Raycast::Single(Vector3(0.0f, 0.0f, 10.0f), Backward, hit, 100.0f));
        EXPECT_EQ(hit.bodyHandle, hits[0].bodyHandle);
    }

    TYPED_TEST(PhysicsQueryTest, HitsThatTieOnOneBodyGoToTheFirstCollider)
    {
        // A box and a sphere on one body whose surfaces are both 4.5 away along the ray (the box was added first)
        const auto go = this->Spawn("Pair", Vector3(0.0f, 0.0f, 5.0f));
        auto *box = go->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *sphere = go->template AddComponent<SphereCollider>();
        sphere->SetRadius(0.5f);
        go->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_EQ(hit.collider, box) << "the collider registered first wins a tie";
        EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);

        std::vector<RaycastHit> hits;
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f), 2) << "both colliders are hit, once each";
    }

    TYPED_TEST(PhysicsQueryTest, RaycastStartingInsideAColliderHitsItAtDistanceZero)
    {
        const auto room = this->SpawnBox("Room", Vector3::Zero, 4.0f);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3(0.5f, 0.0f, 0.0f), Right, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, room.get());
        EXPECT_FLOAT_EQ(hit.distance, 0.0f);
        ExpectVectorNear(hit.point, Vector3(0.5f, 0.0f, 0.0f), 1e-5f);
        ExpectVectorNear(hit.normal, Vector3(-1.0f, 0.0f, 0.0f), 1e-5f);

        std::vector<RaycastHit> hits;
        ASSERT_EQ(Raycast::All(Vector3(0.5f, 0.0f, 0.0f), Right, hits, 100.0f), 1) << "the way out is not a hit";
        EXPECT_FLOAT_EQ(hits[0].distance, 0.0f);
    }

    TYPED_TEST(PhysicsQueryTest, QueriesReportTheDynamicBodyAndTheColliderThatWasHit)
    {
        // One body, two colliders: a box at the origin and a sphere off to the side
        const auto compound = this->Spawn("Compound", Vector3::Zero);
        auto *box = compound->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *sphere = compound->template AddComponent<SphereCollider>();
        sphere->SetRadius(0.5f);
        sphere->SetOffset(Vector3(5.0f, 0.0f, 0.0f));
        auto *body = this->AddFloatingBody(*compound, 2.0f);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3(5.0f, 5.0f, 0.0f), Down, hit, 100.0f));
        EXPECT_EQ(hit.collider, sphere);
        EXPECT_EQ(hit.rigidbody, body);
        EXPECT_EQ(hit.gameObject, compound.get());
        EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);

        ASSERT_TRUE(Raycast::Single(Vector3(0.0f, 5.0f, 0.0f), Down, hit, 100.0f));
        EXPECT_EQ(hit.collider, box);

        ASSERT_TRUE(Raycast::SphereCast(Vector3(5.0f, 5.0f, 0.0f), 0.1f, Down, hit, 100.0f));
        EXPECT_EQ(hit.collider, sphere);
        ASSERT_TRUE(Raycast::SphereCast(Vector3(0.0f, 5.0f, 0.0f), 0.1f, Down, hit, 100.0f));
        EXPECT_EQ(hit.collider, box);
        EXPECT_EQ(hit.rigidbody, body);

        ASSERT_TRUE(Raycast::CapsuleCast(Vector3(5.0f, 5.0f, 0.0f), Vector3(5.0f, 6.0f, 0.0f), 0.1f, Down, hit, 100.0f));
        EXPECT_EQ(hit.collider, sphere);

        // A hit reused for a body with no Rigidbody keeps nothing of the last one
        const auto wall = this->SpawnBox("Wall", Vector3(-5.0f, 0.0f, 0.0f));
        this->Step();
        ASSERT_TRUE(Raycast::Single(Vector3(-5.0f, 5.0f, 0.0f), Down, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, wall.get());
        EXPECT_EQ(hit.rigidbody, nullptr);
    }

    // ============================================================================
    // SphereCast
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, SphereCastStopsWhenItsSurfaceTouches)
    {
        const auto target = this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, target.get());
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance) << "the sphere's centre travels until its surface touches";
        ExpectVectorNear(hit.point, Vector3(0.0f, 0.0f, 4.5f), kHitTolerance);
        ExpectVectorNear(hit.normal, Backward, kHitTolerance);

        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 3.5f)) << "too short";
        EXPECT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, kInf)) << "an infinite distance is clamped";
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance);
    }

    TYPED_TEST(PhysicsQueryTest, SphereCastHitsWhatARayAtItsCentreMisses)
    {
        // A box 2 to the side of the path: a ray passes it, a sphere of radius 2 does not, one of radius 1 does
        this->SpawnBox("Beside", Vector3(2.0f, 0.0f, 5.0f));
        this->Step();

        RaycastHit hit;
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 1.0f, Forward, hit, 100.0f));
        EXPECT_TRUE(Raycast::SphereCast(Vector3::Zero, 2.0f, Forward, hit, 100.0f));
    }

    TYPED_TEST(PhysicsQueryTest, SphereCastStartingInsideHitsAtDistanceZero)
    {
        const auto room = this->SpawnBox("Room", Vector3::Zero, 4.0f);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::SphereCast(Vector3(0.25f, 0.0f, 0.0f), 0.5f, Right, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, room.get());
        EXPECT_FLOAT_EQ(hit.distance, 0.0f);
        ExpectVectorNear(hit.point, Vector3(0.25f, 0.0f, 0.0f), 1e-5f);
        ExpectVectorNear(hit.normal, Vector3(-1.0f, 0.0f, 0.0f), 1e-5f);
    }

    TYPED_TEST(PhysicsQueryTest, SphereCastTiesAreOrderedByBodyHandle)
    {
        this->SpawnBox("First", Vector3(0.0f, 0.0f, 5.0f));
        this->SpawnBox("Second", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        std::vector<RaycastHit> rays;
        ASSERT_EQ(Raycast::All(Vector3::Zero, Forward, rays, 100.0f), 2);
        RaycastHit hit;
        ASSERT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 100.0f));
        EXPECT_EQ(hit.bodyHandle, rays[0].bodyHandle) << "the lower handle wins the tie";
    }

    // ============================================================================
    // CapsuleCast
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, CapsuleCastSweepsTheWholeSegment)
    {
        const auto target = this->SpawnBox("Target", Vector3(5.0f, 1.0f, 0.0f));
        this->Step();

        // Standing along y, from the origin: its cylinder wall is 0.5 from the axis
        RaycastHit hit;
        ASSERT_TRUE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 2.0f, 0.0f), 0.5f, Right, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, target.get());
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance);
        ExpectVectorNear(hit.normal, Vector3(-1.0f, 0.0f, 0.0f), kHitTolerance);
        EXPECT_NEAR(hit.point.x, 4.5f, kHitTolerance);

        // Lying along z: the same distance, and a box off its end is not hit
        ASSERT_TRUE(Raycast::CapsuleCast(Vector3(0.0f, 1.0f, -1.0f), Vector3(0.0f, 1.0f, 1.0f), 0.5f, Right, hit,
                                         100.0f));
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance);
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3(0.0f, 1.0f, -5.0f), Vector3(0.0f, 1.0f, -3.0f), 0.5f, Right, hit,
                                          100.0f));

        // The order of the end points does not matter
        ASSERT_TRUE(Raycast::CapsuleCast(Vector3(0.0f, 2.0f, 0.0f), Vector3::Zero, 0.5f, Right, hit, 100.0f));
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance);

        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 2.0f, 0.0f), 0.5f, Right, hit, 3.0f))
            << "too short";
    }

    TYPED_TEST(PhysicsQueryTest, CapsuleCastReachesWithItsEndCaps)
    {
        this->SpawnBox("High", Vector3(5.0f, 4.0f, 0.0f));
        this->Step();

        RaycastHit hit;
        EXPECT_TRUE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 3.5f, 0.0f), 0.5f, Right, hit, 100.0f))
            << "its upper end sphere reaches the box";
        EXPECT_NEAR(hit.distance, 4.0f, kHitTolerance);
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 2.0f, 0.0f), 0.5f, Right, hit, 100.0f))
            << "a shorter one passes underneath";
    }

    TYPED_TEST(PhysicsQueryTest, CapsuleCastOfOnePointIsASphereCast)
    {
        this->SpawnBox("Target", Vector3(5.0f, 1.0f, 0.0f));
        this->Step();

        RaycastHit capsule;
        RaycastHit sphere;
        const Vector3 point(0.0f, 1.0f, 0.0f);
        ASSERT_TRUE(Raycast::CapsuleCast(point, point, 0.5f, Right, capsule, 100.0f));
        ASSERT_TRUE(Raycast::SphereCast(point, 0.5f, Right, sphere, 100.0f));
        EXPECT_NEAR(capsule.distance, 4.0f, kHitTolerance);
        EXPECT_NEAR(capsule.distance, sphere.distance, kHitTolerance);
    }

    TYPED_TEST(PhysicsQueryTest, CapsuleCastStartingInsideHitsAtDistanceZero)
    {
        const auto room = this->SpawnBox("Room", Vector3::Zero, 4.0f);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::CapsuleCast(Vector3(0.0f, -0.5f, 0.0f), Vector3(0.0f, 0.5f, 0.0f), 0.25f, Right, hit,
                                         100.0f));
        EXPECT_EQ(hit.gameObject, room.get());
        EXPECT_FLOAT_EQ(hit.distance, 0.0f);
        ExpectVectorNear(hit.point, Vector3::Zero, 1e-5f); // the centre of the capsule
        ExpectVectorNear(hit.normal, Vector3(-1.0f, 0.0f, 0.0f), 1e-5f);
    }

    // ============================================================================
    // Overlaps
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, OverlapSphereFindsEveryColliderInReachSortedByHandle)
    {
        const auto a = this->SpawnBox("A", Vector3(0.0f, 0.0f, 0.0f));
        const auto b = this->SpawnBox("B", Vector3(3.0f, 0.0f, 0.0f));
        this->SpawnBox("C", Vector3(10.0f, 0.0f, 0.0f));
        this->Step();

        std::vector<OverlapHit> hits(4); // the previous content is replaced
        ASSERT_EQ(Overlap::Sphere(Vector3(1.5f, 0.0f, 0.0f), 2.0f, hits), 2);
        ASSERT_EQ(hits.size(), 2u);
        EXPECT_EQ(TestFixture::ObjectsOf(hits), (std::set<const GameObject *>{a.get(), b.get()}));
        EXPECT_TRUE(TestFixture::IsSortedByHandle(hits));
        for (const OverlapHit &hit : hits)
        {
            EXPECT_TRUE(hit.bodyHandle.IsValid());
            EXPECT_NE(hit.collider, nullptr);
            EXPECT_EQ(hit.rigidbody, nullptr);
        }

        EXPECT_EQ(Overlap::Sphere(Vector3(1.5f, 0.0f, 0.0f), 0.2f, hits), 0) << "nothing within reach";
        EXPECT_TRUE(hits.empty());
        EXPECT_EQ(Overlap::Sphere(Vector3(10.0f, 0.0f, 0.0f), 0.2f, hits), 1);
    }

    TYPED_TEST(PhysicsQueryTest, OverlapSphereInsideAColliderFindsIt)
    {
        const auto room = this->SpawnBox("Room", Vector3::Zero, 4.0f);
        this->Step();

        std::vector<OverlapHit> hits;
        ASSERT_EQ(Overlap::Sphere(Vector3::Zero, 0.1f, hits), 1) << "a small sphere deep inside is an overlap";
        EXPECT_EQ(hits[0].gameObject, room.get());
    }

    TYPED_TEST(PhysicsQueryTest, OverlapBoxFollowsItsRotation)
    {
        const auto a = this->SpawnBox("A", Vector3(3.0f, 0.0f, 0.0f));
        const auto b = this->SpawnBox("B", Vector3(10.0f, 0.0f, 0.0f));
        this->Step();

        // Long along z at x = 7: it overlaps nothing; turned a quarter about y it is long along x and reaches both
        std::vector<OverlapHit> hits;
        const Vector3 center(7.0f, 0.0f, 0.0f);
        const Vector3 half(0.2f, 0.2f, 5.0f);
        EXPECT_EQ(Overlap::Box(center, half, Math::Quaternion::Identity, hits), 0);

        const Math::Quaternion quarterAboutY(0.70710678f, 0.0f, 0.70710678f, 0.0f); // (w, x, y, z)
        ASSERT_EQ(Overlap::Box(center, half, quarterAboutY, hits), 2);
        EXPECT_EQ(TestFixture::ObjectsOf(hits), (std::set<const GameObject *>{a.get(), b.get()}));
        EXPECT_TRUE(TestFixture::IsSortedByHandle(hits));

        // A rotation that is not of unit length is normalised
        const Math::Quaternion scaled(1.41421356f, 0.0f, 1.41421356f, 0.0f);
        EXPECT_EQ(Overlap::Box(center, half, scaled, hits), 2);
    }

    TYPED_TEST(PhysicsQueryTest, OverlapCapsuleUsesItsSegment)
    {
        const auto target = this->SpawnBox("Target", Vector3(3.0f, 0.0f, 0.0f));
        this->SpawnBox("Other", Vector3(8.0f, 0.0f, 0.0f));
        this->Step();

        // Standing along y at x = 3: its segment is 4 long, so only the box beside it is touched
        std::vector<OverlapHit> hits;
        ASSERT_EQ(Overlap::Capsule(Vector3(3.0f, -2.0f, 0.0f), Vector3(3.0f, 2.0f, 0.0f), 0.5f, hits), 1);
        EXPECT_EQ(hits[0].gameObject, target.get());

        // Lying along x it reaches both
        EXPECT_EQ(Overlap::Capsule(Vector3(2.0f, 0.0f, 0.0f), Vector3(8.0f, 0.0f, 0.0f), 0.2f, hits), 2);
        // Above the boxes it reaches none
        EXPECT_EQ(Overlap::Capsule(Vector3(3.0f, 3.0f, 0.0f), Vector3(3.0f, 5.0f, 0.0f), 0.5f, hits), 0);

        // Equal end points make a sphere
        ASSERT_EQ(Overlap::Capsule(Vector3(3.0f, 0.0f, 1.0f), Vector3(3.0f, 0.0f, 1.0f), 0.8f, hits), 1);
        EXPECT_EQ(hits[0].gameObject, target.get());
    }

    TYPED_TEST(PhysicsQueryTest, AnOverlapReportsEachColliderOnceInColliderOrder)
    {
        const auto compound = this->Spawn("Compound", Vector3::Zero);
        auto *box = compound->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *sphere = compound->template AddComponent<SphereCollider>();
        sphere->SetRadius(0.5f);
        sphere->SetOffset(Vector3(5.0f, 0.0f, 0.0f));
        auto *body = this->AddFloatingBody(*compound, 1.0f);
        this->SpawnBox("Alone", Vector3(0.0f, 0.0f, 20.0f));
        this->Step();

        std::vector<OverlapHit> hits;
        ASSERT_EQ(Overlap::Sphere(Vector3(2.5f, 0.0f, 0.0f), 4.0f, hits), 2) << "two colliders, one body";
        EXPECT_EQ(hits[0].bodyHandle, hits[1].bodyHandle);
        EXPECT_EQ(hits[0].collider, box) << "the collider added first comes first";
        EXPECT_EQ(hits[1].collider, sphere);
        EXPECT_EQ(hits[0].rigidbody, body);
        EXPECT_EQ(hits[0].gameObject, compound.get());

        ASSERT_EQ(Overlap::Sphere(Vector3(5.0f, 0.0f, 0.0f), 0.2f, hits), 1) << "only the sphere is in reach";
        EXPECT_EQ(hits[0].collider, sphere);
    }

    // ============================================================================
    // Degenerate input
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, DegenerateInputFindsNothing)
    {
        // A box right in the path of every query below, so a query that ignored its bad input would hit it
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f), 2.0f);
        this->Step();

        const Vector3 nan3(kNaN, 0.0f, 1.0f);
        const Vector3 inf3(0.0f, kInf, 0.0f);
        RaycastHit hit;
        std::vector<RaycastHit> hits(2);
        std::vector<OverlapHit> overlaps(2);

        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f)) << "the setup must be hittable";

        // Rays
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Vector3::Zero, hit, 100.0f)) << "zero direction";
        EXPECT_FALSE(hit.hit);
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, nan3, hit, 100.0f)) << "NaN direction";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, inf3, hit, 100.0f)) << "infinite direction";
        EXPECT_FALSE(Raycast::Single(nan3, Forward, hit, 100.0f)) << "NaN origin";
        EXPECT_FALSE(Raycast::Single(inf3, Forward, hit, 100.0f)) << "infinite origin";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 0.0f)) << "zero distance";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, -1.0f)) << "negative distance";
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, kNaN)) << "NaN distance";
        EXPECT_EQ(Raycast::All(Vector3::Zero, Vector3::Zero, hits, 100.0f), 0);
        EXPECT_TRUE(hits.empty());
        hits.resize(2);
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 0.0f), 0);
        EXPECT_TRUE(hits.empty());
        hits.resize(2);
        EXPECT_EQ(Raycast::All(nan3, Forward, hits, 100.0f), 0);
        EXPECT_TRUE(hits.empty());

        // Sphere casts
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.0f, Forward, hit, 100.0f)) << "zero radius";
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, -1.0f, Forward, hit, 100.0f)) << "negative radius";
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, kNaN, Forward, hit, 100.0f)) << "NaN radius";
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, kInf, Forward, hit, 100.0f)) << "infinite radius";
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.5f, Vector3::Zero, hit, 100.0f)) << "zero direction";
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 0.0f)) << "zero distance";
        EXPECT_FALSE(Raycast::SphereCast(nan3, 0.5f, Forward, hit, 100.0f)) << "NaN origin";
        EXPECT_FALSE(hit.hit);

        // Capsule casts
        const Vector3 top(0.0f, 1.0f, 0.0f);
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, top, 0.0f, Forward, hit, 100.0f)) << "zero radius";
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, top, kNaN, Forward, hit, 100.0f)) << "NaN radius";
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, top, 0.5f, Vector3::Zero, hit, 100.0f)) << "zero direction";
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, top, 0.5f, Forward, hit, -1.0f)) << "negative distance";
        EXPECT_FALSE(Raycast::CapsuleCast(nan3, top, 0.5f, Forward, hit, 100.0f)) << "NaN end point";
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, inf3, 0.5f, Forward, hit, 100.0f)) << "infinite end point";
        EXPECT_FALSE(hit.hit);

        // Overlaps, each at the box so that a bad shape would find it
        const Vector3 at(0.0f, 0.0f, 5.0f);
        EXPECT_EQ(Overlap::Sphere(at, 0.0f, overlaps), 0) << "zero radius";
        EXPECT_TRUE(overlaps.empty());
        overlaps.resize(2);
        EXPECT_EQ(Overlap::Sphere(at, kNaN, overlaps), 0);
        EXPECT_TRUE(overlaps.empty());
        overlaps.resize(2);
        EXPECT_EQ(Overlap::Sphere(nan3, 1.0f, overlaps), 0) << "NaN centre";
        EXPECT_EQ(Overlap::Box(at, Vector3(0.0f, 1.0f, 1.0f), Math::Quaternion::Identity, overlaps), 0)
            << "a flat box";
        EXPECT_EQ(Overlap::Box(at, Vector3(1.0f, -1.0f, 1.0f), Math::Quaternion::Identity, overlaps), 0);
        EXPECT_EQ(Overlap::Box(at, Vector3(1.0f, 1.0f, 1.0f), Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f), overlaps), 0)
            << "a rotation with no length";
        EXPECT_EQ(Overlap::Capsule(at, at, 0.0f, overlaps), 0) << "zero radius";
        EXPECT_EQ(Overlap::Capsule(nan3, at, 1.0f, overlaps), 0) << "NaN end point";

        // None of that left anything behind: the box is still there
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_EQ(Overlap::Sphere(at, 0.1f, overlaps), 1);
    }

    // ============================================================================
    // Layer masks
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, LayerMasksSelectWhatEveryQueryCanHit)
    {
        const auto nineBox = this->SpawnBox("OnNine", Vector3(0.0f, 0.0f, 5.0f));
        nineBox->SetLayer(9);
        const auto tenBox = this->SpawnBox("OnTen", Vector3(0.0f, 0.0f, 9.0f));
        tenBox->SetLayer(10);
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(10)));
        EXPECT_EQ(hit.gameObject, tenBox.get()) << "the nearer box is on another layer";
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(9)));
        EXPECT_EQ(hit.gameObject, nineBox.get());
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(9) | Layers::MaskOf(10)));
        EXPECT_EQ(hit.gameObject, nineBox.get());
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(11)));
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, 0u)) << "an empty mask hits nothing";

        std::vector<RaycastHit> hits;
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f, Layers::MaskOf(10)), 1);
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f, Layers::AllLayers), 2);
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f, 0u), 0);

        ASSERT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 100.0f, Layers::MaskOf(10)));
        EXPECT_EQ(hit.gameObject, tenBox.get());
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.5f, Forward, hit, 100.0f, 0u));

        ASSERT_TRUE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 1.0f, 0.0f), 0.5f, Forward, hit, 100.0f,
                                         Layers::MaskOf(10)));
        EXPECT_EQ(hit.gameObject, tenBox.get());
        EXPECT_FALSE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 1.0f, 0.0f), 0.5f, Forward, hit, 100.0f, 0u));

        std::vector<OverlapHit> overlaps;
        const Vector3 middle(0.0f, 0.0f, 7.0f);
        EXPECT_EQ(Overlap::Sphere(middle, 3.0f, overlaps, Layers::AllLayers), 2);
        ASSERT_EQ(Overlap::Sphere(middle, 3.0f, overlaps, Layers::MaskOf(9)), 1);
        EXPECT_EQ(overlaps[0].gameObject, nineBox.get());
        EXPECT_EQ(Overlap::Sphere(middle, 3.0f, overlaps, 0u), 0);
        EXPECT_EQ(Overlap::Box(middle, Vector3(2.0f, 2.0f, 3.0f), Math::Quaternion::Identity, overlaps,
                               Layers::MaskOf(10)), 1);
        EXPECT_EQ(Overlap::Capsule(Vector3(0.0f, 0.0f, 4.0f), Vector3(0.0f, 0.0f, 10.0f), 0.5f, overlaps,
                                   Layers::MaskOf(11)), 0);
    }

    TYPED_TEST(PhysicsQueryTest, TheDefaultMaskLeavesOutIgnoreRaycast)
    {
        const auto hidden = this->SpawnBox("Hidden", Vector3(0.0f, 0.0f, 5.0f));
        hidden->SetLayer(Layers::IgnoreRaycast);
        this->Step();

        RaycastHit hit;
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::AllLayers));
        std::vector<OverlapHit> overlaps;
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 1.0f, overlaps), 0);
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 1.0f, overlaps, Layers::AllLayers), 1);
    }

    TYPED_TEST(PhysicsQueryTest, ALayerChangedAfterTheBodyExistsIsUsedByTheNextQuery)
    {
        const auto box = this->SpawnBox("Mover", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        RaycastHit hit;
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(9)));
        box->SetLayer(9);
        this->Step();
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(9)));
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::MaskOf(Layers::Default)));
    }

    // ============================================================================
    // Triggers and disabled bodies
    // ============================================================================

    TYPED_TEST(PhysicsQueryTest, IgnoredTriggersAreGoneThroughByEveryQuery)
    {
        // A trigger sphere in front of a solid box
        const auto zone = this->Spawn("Zone", Vector3(0.0f, 0.0f, 5.0f));
        auto *trigger = zone->template AddComponent<SphereCollider>();
        trigger->SetRadius(1.0f);
        trigger->SetIsTrigger(true);
        const auto wall = this->SpawnBox("Wall", Vector3(0.0f, 0.0f, 9.0f));
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f, Layers::DefaultRaycastMask,
                                    QueryTriggers::Ignore));
        EXPECT_EQ(hit.gameObject, wall.get()) << "the ray goes through the trigger";
        EXPECT_NEAR(hit.distance, 8.5f, 1e-3f);

        std::vector<RaycastHit> hits;
        ASSERT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f, Layers::DefaultRaycastMask,
                               QueryTriggers::Ignore), 1);
        EXPECT_EQ(hits[0].gameObject, wall.get());

        ASSERT_TRUE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 0.5f, 0.0f), 0.25f, Forward, hit, 100.0f,
                                         Layers::DefaultRaycastMask, QueryTriggers::Ignore));
        EXPECT_EQ(hit.gameObject, wall.get());

        std::vector<OverlapHit> overlaps;
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 0.5f, overlaps, Layers::DefaultRaycastMask,
                                  QueryTriggers::Ignore), 0);
        EXPECT_EQ(Overlap::Box(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.5f, 0.5f, 0.5f), Math::Quaternion::Identity,
                               overlaps, Layers::DefaultRaycastMask, QueryTriggers::Ignore), 0);
        EXPECT_EQ(Overlap::Capsule(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.5f, 5.0f), 0.25f, overlaps,
                                   Layers::DefaultRaycastMask, QueryTriggers::Ignore), 0);
    }

    TYPED_TEST(PhysicsQueryTest, TriggersAreHitUnlessIgnored)
    {
        // On Jolt a trigger collider is a sub-shape of its body's shape (J4), so the queries hit it as on PhysX
        const auto zone = this->Spawn("Zone", Vector3(0.0f, 0.0f, 5.0f));
        auto *trigger = zone->template AddComponent<SphereCollider>();
        trigger->SetRadius(1.0f);
        trigger->SetIsTrigger(true);
        this->SpawnBox("Wall", Vector3(0.0f, 0.0f, 9.0f));
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, zone.get());
        EXPECT_EQ(hit.collider, trigger);
        EXPECT_NEAR(hit.distance, 4.0f, 1e-3f);
        std::vector<RaycastHit> hits;
        EXPECT_EQ(Raycast::All(Vector3::Zero, Forward, hits, 100.0f), 2) << "the trigger and the wall behind it";
        std::vector<OverlapHit> overlaps;
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 0.5f, overlaps), 1);
        ASSERT_TRUE(Raycast::CapsuleCast(Vector3::Zero, Vector3(0.0f, 0.5f, 0.0f), 0.25f, Forward, hit, 100.0f));
        EXPECT_EQ(hit.gameObject, zone.get());
    }

    TYPED_TEST(PhysicsQueryTest, ADisabledBodyIsNotQueriedAndComesBack)
    {
        const auto ball = this->Spawn("Ball", Vector3(0.0f, 0.0f, 5.0f));
        ball->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        this->AddFloatingBody(*ball, 1.0f);
        this->Step();

        RaycastHit hit;
        std::vector<OverlapHit> overlaps;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        ASSERT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 0.2f, overlaps), 1);

        ball->SetActive(false);
        this->Step();
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.2f, Forward, hit, 100.0f));
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 0.2f, overlaps), 0);

        ball->SetActive(true);
        this->Step();
        EXPECT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_EQ(Overlap::Sphere(Vector3(0.0f, 0.0f, 5.0f), 0.2f, overlaps), 1);
    }

    TYPED_TEST(PhysicsQueryTest, QueriesFollowAMovedStaticCollider)
    {
        const auto box = this->SpawnBox("Mover", Vector3(0.0f, 0.0f, 5.0f));
        this->Step();

        RaycastHit hit;
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        box->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, 8.0f));
        this->Step();
        ASSERT_TRUE(Raycast::Single(Vector3::Zero, Forward, hit, 100.0f));
        EXPECT_NEAR(hit.distance, 7.5f, 1e-3f);
    }
}

#endif // a physics backend is in this build
