#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>

#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Camera.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/picking/ScenePicking.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// CPU picking and world bounds (#80, E7b): IRenderable::GetWorldBounds of each renderer, the ray tests, and
// PickGameObject / GetGameObjectBounds over scenes that were never attached (edit mode). No window, GPU or physics.

using namespace N2Engine;
using Math::Quaternion;
using Math::Ray;
using Math::Vector2;
using Math::Vector3;

namespace
{
    constexpr float Tolerance = 1e-3f;

    /// From (x, y, z) straight down -z, as a camera on the z axis looks
    Ray Down(const float x, const float y, const float z = 10.0f)
    {
        return Ray(Vector3(x, y, z), Vector3(0.0f, 0.0f, -1.0f));
    }

    void ExpectVector(const Vector3 &actual, const Vector3 &expected, const char *what)
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.z, expected.z, Tolerance) << what << " z";
    }

    void ExpectBox(const std::optional<BoundingBox> &box, const Vector3 &min, const Vector3 &max, const char *what)
    {
        ASSERT_TRUE(box.has_value()) << what;
        ExpectVector(box->min, min, what);
        ExpectVector(box->max, max, what);
    }

    GameObject::Ptr AddCube(Scene &scene, const std::string &name, const Vector3 &position,
                            const Vector3 &size = Vector3(1.0f, 1.0f, 1.0f))
    {
        auto object = GameObject::Create(name);
        auto *cube = object->AddComponent<Example::CubeRenderer>();
        cube->SetSize(size);
        object->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(object);
        return object;
    }

    GameObject::Ptr AddEmpty(Scene &scene, const std::string &name, const Vector3 &position)
    {
        auto object = GameObject::Create(name);
        object->CreatePositionable();
        object->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(object);
        return object;
    }

    /// A scene in edit mode: its components never attach, as in the editor
    std::unique_ptr<Scene> EditScene(const std::string &name)
    {
        auto scene = Scene::Create(name);
        scene->SetEditMode(true);
        return scene;
    }

    /// A child element at a fixed rect inside its parent (bottom-left anchors and pivot), with an Image
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

    /// A world canvas of 100 x 100 canvas units (1 x 1 world units at 0.01), its centre at `position`
    GameObject::Ptr AddWorldCanvas(Scene &scene, const Vector3 &position)
    {
        auto canvas = UI::UISystem::CreateCanvas("Canvas", UI::CanvasRenderMode::WorldSpace);
        canvas->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(canvas);
        return canvas;
    }
}

// ==================== The ray tests ====================

TEST(PickingRayTest, ABoxIsEnteredAtItsNearFace)
{
    const BoundingBox box(Vector3(-1.0f, -1.0f, -1.0f), Vector3(1.0f, 1.0f, 1.0f));
    const auto hit = Picking::RaycastBox(Down(0.0f, 0.0f, 5.0f), box);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(*hit, 4.0f, Tolerance);

    EXPECT_FALSE(Picking::RaycastBox(Down(2.0f, 0.0f, 5.0f), box).has_value()) << "beside it";
    EXPECT_FALSE(Picking::RaycastBox(Down(0.0f, 0.0f, -5.0f), box).has_value()) << "it is behind the ray";
    EXPECT_FALSE(Picking::RaycastBox(Down(0.0f, 0.0f, 5.0f), box, 3.0f).has_value()) << "beyond the distance asked";

    const auto inside = Picking::RaycastBox(Down(0.0f, 0.0f, 0.0f), box);
    ASSERT_TRUE(inside.has_value());
    EXPECT_NEAR(*inside, 0.0f, Tolerance) << "a ray that starts inside is at distance 0";
}

TEST(PickingRayTest, ARayParallelToAFacePairHitsOnlyInsideItsSlab)
{
    const BoundingBox box(Vector3(-1.0f, -1.0f, -1.0f), Vector3(1.0f, 1.0f, 1.0f));
    // Along +x at y = 0: through the box; at y = 2: past it
    EXPECT_TRUE(Picking::RaycastBox(Ray(Vector3(-5.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f)), box).has_value());
    EXPECT_FALSE(Picking::RaycastBox(Ray(Vector3(-5.0f, 2.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f)), box).has_value());
}

TEST(PickingRayTest, ASphereIsEnteredAtItsNearSide)
{
    const auto hit = Picking::RaycastSphere(Down(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(*hit, 4.0f, Tolerance);

    EXPECT_FALSE(Picking::RaycastSphere(Down(1.5f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f).has_value());
    EXPECT_FALSE(Picking::RaycastSphere(Down(0.0f, 0.0f, -5.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f).has_value())
        << "behind the ray";
    const auto inside = Picking::RaycastSphere(Down(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f);
    ASSERT_TRUE(inside.has_value());
    EXPECT_NEAR(*inside, 0.0f, Tolerance);
    EXPECT_FALSE(Picking::RaycastSphere(Down(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f, 3.0f).has_value());
}

TEST(PickingRayTest, AMeshIsHitOnItsTrianglesNotItsBox)
{
    const auto cube = Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube);
    const auto identity = Math::Matrix<float, 4, 4>::identity();
    const auto front = Picking::RaycastMesh(*cube, identity, Down(0.0f, 0.0f, 5.0f));
    ASSERT_TRUE(front.has_value());
    EXPECT_NEAR(*front, 4.5f, Tolerance);

    // From inside, the back face is hit too (either face counts)
    const auto fromInside = Picking::RaycastMesh(*cube, identity, Down(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(fromInside.has_value());
    EXPECT_NEAR(*fromInside, 0.5f, Tolerance);

    EXPECT_FALSE(Picking::RaycastMesh(*cube, identity, Down(0.7f, 0.0f, 5.0f)).has_value());

    // The sphere's box has corners the sphere doesn't fill: a ray through one passes the box and misses the mesh
    const auto sphere = Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Sphere);
    EXPECT_TRUE(Picking::RaycastBox(Down(0.4f, 0.4f, 5.0f), sphere->GetBounds()).has_value());
    EXPECT_FALSE(Picking::RaycastMesh(*sphere, identity, Down(0.4f, 0.4f, 5.0f)).has_value());
    const auto middle = Picking::RaycastMesh(*sphere, identity, Down(0.0f, 0.0f, 5.0f));
    ASSERT_TRUE(middle.has_value());
    EXPECT_NEAR(*middle, 4.5f, 0.02f);
}

TEST(PickingRayTest, AMeshModelMovesAndScalesTheTriangles)
{
    const auto cube = Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube);
    // Scaled to 4 wide and moved to z = -3: the near face is at z = -1
    const auto model = Math::Matrix<float, 4, 4>::Translation(Vector3(0.0f, 0.0f, -3.0f)) *
                       Math::Matrix<float, 4, 4>::Scale(4.0f, 4.0f, 4.0f);
    const auto hit = Picking::RaycastMesh(*cube, model, Down(1.5f, 0.0f, 5.0f));
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(*hit, 6.0f, Tolerance) << "the distance is in world units, whatever the scale";
    EXPECT_FALSE(Picking::RaycastMesh(*cube, model, Down(2.5f, 0.0f, 5.0f)).has_value());
}

// ==================== GetWorldBounds ====================

TEST(WorldBoundsTest, AShapeIsItsMeshScaledBySizeAndMovedByTheTransform)
{
    auto object = GameObject::Create("Cube");
    auto *cube = object->AddComponent<Example::CubeRenderer>();
    cube->SetSize(Vector3(2.0f, 4.0f, 6.0f));
    object->GetPositionable()->SetPosition(Vector3(1.0f, 2.0f, 3.0f));

    ExpectBox(cube->GetWorldBounds(), Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 6.0f), "cube");

    auto quad = GameObject::Create("Quad");
    auto *quadRenderer = quad->AddComponent<Example::QuadRenderer>();
    ExpectBox(quadRenderer->GetWorldBounds(), Vector3(-0.5f, -0.5f, 0.0f), Vector3(0.5f, 0.5f, 0.0f), "quad");

    auto ball = GameObject::Create("Sphere");
    auto *sphere = ball->AddComponent<Example::SphereRenderer>();
    ExpectBox(sphere->GetWorldBounds(), Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f), "sphere");
}

TEST(WorldBoundsTest, TheHierarchyScalesAndMovesAChildsBounds)
{
    auto scene = EditScene("WorldBounds_Hierarchy");
    auto parent = AddEmpty(*scene, "Parent", Vector3(10.0f, 0.0f, 0.0f));
    parent->GetPositionable()->SetLocalScale(Vector3(2.0f, 2.0f, 2.0f));
    auto child = GameObject::Create("Child");
    auto *cube = child->AddComponent<Example::CubeRenderer>();
    parent->AddChild(child, false);
    child->GetPositionable()->SetLocalPosition(Vector3(1.0f, 0.0f, 0.0f));

    // World position (12, 0, 0), scale 2: the unit cube spans 11 to 13 on x and -1 to 1 on y and z
    ExpectBox(cube->GetWorldBounds(), Vector3(11.0f, -1.0f, -1.0f), Vector3(13.0f, 1.0f, 1.0f), "child cube");
}

TEST(WorldBoundsTest, ARotatedShapeHasTheBoxAroundItsRotatedMesh)
{
    auto object = GameObject::Create("Plank");
    auto *cube = object->AddComponent<Example::CubeRenderer>();
    cube->SetSize(Vector3(2.0f, 1.0f, 1.0f));
    object->GetPositionable()->SetRotation(
        Quaternion::FromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), std::numbers::pi_v<float> / 2.0f));
    // A quarter turn about z swaps the x and y extents
    ExpectBox(cube->GetWorldBounds(), Vector3(-0.5f, -1.0f, -0.5f), Vector3(0.5f, 1.0f, 0.5f), "plank");
}

TEST(WorldBoundsTest, AMeshRendererNeedsAMesh)
{
    auto object = GameObject::Create("Mesh");
    auto *renderer = object->AddComponent<Rendering::MeshRenderer>();
    EXPECT_FALSE(renderer->GetWorldBounds().has_value());

    renderer->SetMesh(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube));
    object->GetPositionable()->SetPosition(Vector3(0.0f, 5.0f, 0.0f));
    ExpectBox(renderer->GetWorldBounds(), Vector3(-0.5f, 4.5f, -0.5f), Vector3(0.5f, 5.5f, 0.5f), "mesh");
}

TEST(WorldBoundsTest, TextIsItsLaidOutRectangleInTheObjectsPlane)
{
    auto object = GameObject::Create("Text");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    EXPECT_FALSE(text->GetWorldBounds().has_value()) << "empty text has no bounds";

    text->SetText("Hello");
    object->GetPositionable()->SetPosition(Vector3(5.0f, 0.0f, 2.0f));
    const Text::Rect layout = text->GetLayout().bounds;
    ASSERT_FALSE(layout.IsEmpty());

    const auto bounds = text->GetWorldBounds();
    ASSERT_TRUE(bounds.has_value());
    EXPECT_NEAR(bounds->min.x, 5.0f + layout.minX, Tolerance);
    EXPECT_NEAR(bounds->max.x, 5.0f + layout.maxX, Tolerance);
    EXPECT_NEAR(bounds->min.y, layout.minY, Tolerance);
    EXPECT_NEAR(bounds->max.y, layout.maxY, Tolerance);
    EXPECT_NEAR(bounds->min.z, 2.0f, Tolerance);
    EXPECT_NEAR(bounds->max.z, 2.0f, Tolerance);
}

TEST(WorldBoundsTest, AWorldCanvasCoversItsRectAndTheGraphicsOutsideIt)
{
    auto scene = EditScene("WorldBounds_Canvas");
    auto canvas = AddWorldCanvas(*scene, Vector3(0.0f, 0.0f, 0.0f));
    auto *component = canvas->GetComponent<UI::Canvas>();
    // 100 x 100 canvas units at 0.01: one world unit, centred on the object
    ExpectBox(component->GetWorldBounds(), Vector3(-0.5f, -0.5f, 0.0f), Vector3(0.5f, 0.5f, 0.0f), "canvas");

    // A graphic that sticks out to 200 units (1.5 world units from the centre) still draws, so it is in the box
    AddPanel(canvas, "Out", UI::Rect{150.0f, 0.0f, 50.0f, 50.0f});
    ExpectBox(component->GetWorldBounds(), Vector3(-0.5f, -0.5f, 0.0f), Vector3(1.5f, 0.5f, 0.0f), "canvas with panel");

    // An overlay canvas isn't in the world
    auto overlay = UI::UISystem::CreateCanvas("Overlay");
    scene->AddRootGameObject(overlay);
    EXPECT_FALSE(overlay->GetComponent<UI::Canvas>()->GetWorldBounds().has_value());
}

// ==================== PickGameObject ====================

TEST(PickingTest, ACubeIsPickedAtItsNearFaceAndMissedBesideIt)
{
    auto scene = EditScene("Pick_Cube");
    const auto cube = AddCube(*scene, "Cube", Vector3(0.0f, 0.0f, 0.0f));

    const Picking::PickHit hit = Picking::PickGameObject(*scene, Down(0.2f, 0.1f));
    ASSERT_EQ(hit.gameObject, cube.get());
    EXPECT_NEAR(hit.distance, 9.5f, Tolerance);
    ExpectVector(hit.point, Vector3(0.2f, 0.1f, 0.5f), "point");

    const Picking::PickHit miss = Picking::PickGameObject(*scene, Down(0.8f, 0.0f));
    EXPECT_EQ(miss.gameObject, nullptr);
}

TEST(PickingTest, TheNearestObjectWinsWhateverTheHierarchyOrder)
{
    auto scene = EditScene("Pick_Nearest");
    const auto farCube = AddCube(*scene, "Far", Vector3(0.0f, 0.0f, -3.0f));
    const auto nearCube = AddCube(*scene, "Near", Vector3(0.0f, 0.0f, 3.0f));

    const Picking::PickHit hit = Picking::PickGameObject(*scene, Down(0.0f, 0.0f));
    EXPECT_EQ(hit.gameObject, nearCube.get());
    EXPECT_NEAR(hit.distance, 6.5f, Tolerance);

    // From the other side the order turns round
    const Picking::PickHit back =
        Picking::PickGameObject(*scene, Ray(Vector3(0.0f, 0.0f, -10.0f), Vector3(0.0f, 0.0f, 1.0f)));
    EXPECT_EQ(back.gameObject, farCube.get());
}

TEST(PickingTest, ARayThroughTheEmptyCornerOfASpheresBoxPassesToWhatIsBehind)
{
    auto scene = EditScene("Pick_Exact");
    auto ball = GameObject::Create("Ball");
    ball->AddComponent<Example::SphereRenderer>();
    scene->AddRootGameObject(ball);
    const auto wall = AddCube(*scene, "Wall", Vector3(0.0f, 0.0f, -5.0f), Vector3(4.0f, 4.0f, 1.0f));

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, ball.get());
    // Inside the sphere's box, outside the sphere: the wall behind it is what is seen
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.4f, 0.4f)).gameObject, wall.get());
}

TEST(PickingTest, ObjectsWithATransformAndNoShapeAreCaughtByASphere)
{
    auto scene = EditScene("Pick_Sphere");
    const auto empty = AddEmpty(*scene, "Empty", Vector3(3.0f, 0.0f, 0.0f));

    const Picking::PickHit hit = Picking::PickGameObject(*scene, Down(3.1f, 0.1f));
    ASSERT_EQ(hit.gameObject, empty.get());
    EXPECT_NEAR(hit.distance, 10.0f - std::sqrt(Picking::PickSphereRadius * Picking::PickSphereRadius - 0.02f), 0.01f);

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(3.3f, 0.0f)).gameObject, nullptr) << "outside the sphere";
}

TEST(PickingTest, AnObjectWithoutATransformHasNoPlaceInTheWorld)
{
    auto scene = EditScene("Pick_NoTransform");
    auto bare = GameObject::Create("Bare");
    scene->AddRootGameObject(bare);
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, nullptr);
    EXPECT_FALSE(Picking::GetGameObjectBounds(*bare).has_value());
}

TEST(PickingTest, TextIsPickedOnItsRectangle)
{
    auto scene = EditScene("Pick_Text");
    auto object = GameObject::Create("Label");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    text->SetText("Label");
    scene->AddRootGameObject(object);

    const Text::Rect layout = text->GetLayout().bounds;
    ASSERT_FALSE(layout.IsEmpty());
    const float centreX = (layout.minX + layout.maxX) * 0.5f;
    const float centreY = (layout.minY + layout.maxY) * 0.5f;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(centreX, centreY)).gameObject, object.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(layout.maxX + 1.0f, centreY)).gameObject, nullptr);
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(centreX, layout.maxY + 1.0f)).gameObject, nullptr);

    // Empty text has no rectangle, so the object is picked by its sphere at the origin
    text->SetText("");
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.1f, 0.1f)).gameObject, object.get());
}

TEST(PickingTest, ChildrenAreFoundUnderTheirParentsTransform)
{
    auto scene = EditScene("Pick_Child");
    auto parent = AddEmpty(*scene, "Parent", Vector3(0.0f, 10.0f, 0.0f));
    auto child = GameObject::Create("Child");
    child->AddComponent<Example::CubeRenderer>();
    parent->AddChild(child, false);
    child->GetPositionable()->SetLocalPosition(Vector3(5.0f, 0.0f, 0.0f));

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(5.0f, 10.0f)).gameObject, child.get());
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, nullptr);
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.1f, 10.1f)).gameObject, parent.get()) << "the parent's own sphere";
}

TEST(PickingTest, InactiveObjectsAreSkippedUnlessAsked)
{
    auto scene = EditScene("Pick_Inactive");
    const auto cube = AddCube(*scene, "Cube", Vector3(0.0f, 0.0f, 0.0f));
    auto other = AddCube(*scene, "Other", Vector3(0.0f, 0.0f, -4.0f));
    cube->SetActive(false);

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, other.get());

    Picking::PickOptions options;
    options.includeInactive = true;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f), options).gameObject, cube.get());
}

TEST(PickingTest, ObjectsUnderAnInactiveParentAreInactiveToo)
{
    auto scene = EditScene("Pick_InactiveParent");
    auto parent = AddEmpty(*scene, "Parent", Vector3(0.0f, 0.0f, 0.0f));
    auto child = GameObject::Create("Child");
    child->AddComponent<Example::CubeRenderer>();
    parent->AddChild(child, false);
    parent->SetActive(false);

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, nullptr);
    Picking::PickOptions options;
    options.includeInactive = true;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f), options).gameObject, child.get());
}

TEST(PickingTest, ADisabledRendererIsSkippedUnlessAsked)
{
    auto scene = EditScene("Pick_Disabled");
    auto object = GameObject::Create("Cube");
    auto *cube = object->AddComponent<Example::CubeRenderer>();
    scene->AddRootGameObject(object);
    cube->SetActive(false);

    // Skipped as a shape, so the object has none and is caught by its sphere instead
    const Picking::PickHit hit = Picking::PickGameObject(*scene, Down(0.4f, 0.4f));
    EXPECT_EQ(hit.gameObject, nullptr) << "outside the sphere (radius 0.25) at the corner of the cube";
    Picking::PickOptions options;
    options.includeInactive = true;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.4f, 0.4f), options).gameObject, object.get());
}

TEST(PickingTest, TheDistanceLimitCutsOffFarHits)
{
    auto scene = EditScene("Pick_Limit");
    const auto cube = AddCube(*scene, "Cube", Vector3(0.0f, 0.0f, 0.0f));
    Picking::PickOptions options;
    options.maxDistance = 9.0f;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f), options).gameObject, nullptr);
    options.maxDistance = 10.0f;
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f), options).gameObject, cube.get());
}

TEST(PickingTest, APerspectiveCameraRayPicksWhatIsUnderThePixel)
{
    auto scene = EditScene("Pick_Camera");
    const auto left = AddCube(*scene, "Left", Vector3(-2.0f, 0.0f, 0.0f));
    const auto right = AddCube(*scene, "Right", Vector3(2.0f, 0.0f, 0.0f));

    Camera camera;
    camera.SetPerspective(60.0f, 4.0f / 3.0f, 0.1f, 100.0f);
    camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
    const Vector2i viewport{640, 480};

    // At z = 0 the view is 2 * 10 * tan(30 degrees) = 11.5 high and 15.4 wide, so x = 2 is 0.26 of the half width
    // right of the centre: about pixel 320 + 0.26 * 320 = 403 (the cube covers pixels 382 to 424)
    float nearToFar = 0.0f;
    const Ray toRight = camera.ScreenPointToRay(Vector2{403.0f, 240.0f}, viewport, &nearToFar);
    Picking::PickOptions options;
    options.maxDistance = nearToFar;
    EXPECT_EQ(Picking::PickGameObject(*scene, toRight, options).gameObject, right.get());
    const Ray toLeft = camera.ScreenPointToRay(Vector2{237.0f, 240.0f}, viewport, &nearToFar);
    EXPECT_EQ(Picking::PickGameObject(*scene, toLeft, options).gameObject, left.get());
    const Ray toMiddle = camera.ScreenPointToRay(Vector2{320.0f, 240.0f}, viewport, &nearToFar);
    EXPECT_EQ(Picking::PickGameObject(*scene, toMiddle, options).gameObject, nullptr);
}

TEST(PickingTest, AWorldCanvasPicksTheGraphicUnderTheRayAndPassesThroughTheRest)
{
    auto scene = EditScene("Pick_Canvas");
    auto canvas = AddWorldCanvas(*scene, Vector3(0.0f, 0.0f, 0.0f));
    // The left half of the canvas (x -0.5 to 0 in the world) is a panel; the right half is empty. The canvas's own
    // pick sphere (radius 0.25 at its centre) is kept clear of the rays below
    const auto panel = AddPanel(canvas, "Panel", UI::Rect{0.0f, 0.0f, 50.0f, 100.0f});
    const auto wall = AddCube(*scene, "Wall", Vector3(0.0f, 0.0f, -3.0f), Vector3(4.0f, 4.0f, 1.0f));

    const Picking::PickHit onPanel = Picking::PickGameObject(*scene, Down(-0.4f, 0.0f));
    EXPECT_EQ(onPanel.gameObject, panel.get()) << "the graphic's object, not the canvas's";
    EXPECT_NEAR(onPanel.distance, 10.0f, Tolerance);
    ExpectVector(onPanel.point, Vector3(-0.4f, 0.0f, 0.0f), "point on the canvas plane");

    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.4f, 0.0f)).gameObject, wall.get()) << "empty canvas: the wall";
}

TEST(PickingTest, AnOverlayCanvasIsNeverPicked)
{
    auto scene = EditScene("Pick_Overlay");
    auto overlay = UI::UISystem::CreateCanvas("Overlay");
    scene->AddRootGameObject(overlay);
    AddPanel(overlay, "Panel", UI::Rect{0.0f, 0.0f, 100.0f, 100.0f});
    EXPECT_EQ(Picking::PickGameObject(*scene, Down(0.0f, 0.0f)).gameObject, nullptr);
}

// ==================== GetGameObjectBounds ====================

TEST(EntityBoundsTest, AGroupIsTheUnionOfWhatIsUnderIt)
{
    auto scene = EditScene("Bounds_Group");
    auto group = AddEmpty(*scene, "Group", Vector3(100.0f, 100.0f, 100.0f)); // its own pivot is far away
    auto a = GameObject::Create("A");
    a->AddComponent<Example::CubeRenderer>();
    auto b = GameObject::Create("B");
    b->AddComponent<Example::CubeRenderer>();
    group->AddChild(a, false);
    group->AddChild(b, false);
    a->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, 0.0f));
    b->GetPositionable()->SetPosition(Vector3(4.0f, 2.0f, 0.0f));

    ExpectBox(Picking::GetGameObjectBounds(*group), Vector3(-0.5f, -0.5f, -0.5f), Vector3(4.5f, 2.5f, 0.5f),
              "group: the union, not the pivot");
    ExpectBox(Picking::GetGameObjectBounds(*b), Vector3(3.5f, 1.5f, -0.5f), Vector3(4.5f, 2.5f, 0.5f), "child");
}

TEST(EntityBoundsTest, InactiveDescendantsDontCountButTheObjectAskedAboutDoes)
{
    auto scene = EditScene("Bounds_Inactive");
    auto group = AddEmpty(*scene, "Group", Vector3(0.0f, 0.0f, 0.0f));
    auto shown = GameObject::Create("Shown");
    shown->AddComponent<Example::CubeRenderer>();
    auto hidden = GameObject::Create("Hidden");
    hidden->AddComponent<Example::CubeRenderer>();
    group->AddChild(shown, false);
    group->AddChild(hidden, false);
    hidden->GetPositionable()->SetPosition(Vector3(50.0f, 0.0f, 0.0f));
    hidden->SetActive(false);

    ExpectBox(Picking::GetGameObjectBounds(*group), Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f),
              "without the hidden child");
    ExpectBox(Picking::GetGameObjectBounds(*hidden), Vector3(49.5f, -0.5f, -0.5f), Vector3(50.5f, 0.5f, 0.5f),
              "an inactive object asked about is measured");

    // An inactive group is measured too, with its active children
    group->SetActive(false);
    ExpectBox(Picking::GetGameObjectBounds(*group), Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f),
              "inactive group");
}

TEST(EntityBoundsTest, AnObjectWithNothingToMeasureGetsItsPickSphereBox)
{
    auto scene = EditScene("Bounds_Sphere");
    auto empty = AddEmpty(*scene, "Empty", Vector3(1.0f, 2.0f, 3.0f));
    const float r = Picking::PickSphereRadius;
    ExpectBox(Picking::GetGameObjectBounds(*empty), Vector3(1.0f - r, 2.0f - r, 3.0f - r),
              Vector3(1.0f + r, 2.0f + r, 3.0f + r), "empty");
}

TEST(EntityBoundsTest, AUiElementOfAWorldCanvasIsItsRectInTheWorld)
{
    auto scene = EditScene("Bounds_UiElement");
    auto canvas = AddWorldCanvas(*scene, Vector3(0.0f, 0.0f, 0.0f));
    const auto panel = AddPanel(canvas, "Panel", UI::Rect{0.0f, 0.0f, 50.0f, 100.0f});

    ExpectBox(Picking::GetGameObjectBounds(*panel), Vector3(-0.5f, -0.5f, 0.0f), Vector3(0.0f, 0.5f, 0.0f), "panel");
    ExpectBox(Picking::GetGameObjectBounds(*canvas), Vector3(-0.5f, -0.5f, 0.0f), Vector3(0.5f, 0.5f, 0.0f), "canvas");
}
