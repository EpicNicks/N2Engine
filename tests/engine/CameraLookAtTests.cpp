#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>

#include <math/Quaternion.hpp>
#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Camera.hpp"

using namespace N2Engine;
using Math::Vector2;
using Math::Vector3;

// Camera::LookAt (issue #57): the camera looks down its local -Z, so LookAt must point -Z, not +Z, at the
// target. Each test checks where the target lands after the view and projection: the centre of the screen,
// in front of the eye, at a depth inside the clip range.

namespace
{
    constexpr float Tolerance = 1e-3f;
    const Vector2i Viewport{800, 600};

    void ExpectNear(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << ".x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << ".y";
        EXPECT_NEAR(actual.z, expected.z, Tolerance) << what << ".z";
    }

    bool IsFinite(const Matrix4 &m)
    {
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t col = 0; col < 4; ++col)
            {
                if (!std::isfinite(m(row, col)))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsFinite(const Math::Quaternion &q)
    {
        return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    }

    Camera PerspectiveCamera(const Vector3 &position)
    {
        Camera camera;
        camera.SetPerspective(60.0f, 4.0f / 3.0f, 0.1f, 100.0f);
        camera.SetPosition(position);
        return camera;
    }

    // The target must be in front of the eye (view z < 0), at the centre of the screen, inside the depth range
    void ExpectTargetCentredInFront(const Camera &camera, const Vector3 &target, const std::string &what)
    {
        const Vector3 view = camera.GetViewMatrix().TransformPoint(target);
        EXPECT_LT(view.z, 0.0f) << what << ": the target is behind the camera";
        EXPECT_NEAR(view.z, -(target - camera.GetPosition()).Length(), Tolerance) << what << ": view depth";
        EXPECT_NEAR(view.x, 0.0f, Tolerance) << what << ": view x";
        EXPECT_NEAR(view.y, 0.0f, Tolerance) << what << ": view y";

        const Vector3 ndc = camera.GetViewProjectionMatrix().TransformPoint(target);
        EXPECT_NEAR(ndc.x, 0.0f, Tolerance) << what << ": ndc x";
        EXPECT_NEAR(ndc.y, 0.0f, Tolerance) << what << ": ndc y";
        EXPECT_GT(ndc.z, -1.0f) << what << ": ndc depth";
        EXPECT_LT(ndc.z, 1.0f) << what << ": ndc depth";

        EXPECT_NEAR(camera.GetRotation().Length(), 1.0f, Tolerance) << what << ": rotation is a unit quaternion";
        ExpectNear(camera.GetForward(), (target - camera.GetPosition()).Normalized(), what + ": GetForward");
    }
}

TEST(CameraLookAtTest, LookingAtTheOriginFromPositiveZ)
{
    Camera camera = PerspectiveCamera(Vector3(0.0f, 0.0f, 5.0f));
    camera.LookAt(Vector3(0.0f, 0.0f, 0.0f));

    // The OpenGL default: no turn at all, so the origin is 5 units down -Z
    ExpectNear(camera.GetRotation() * Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 0.0f, -1.0f), "rotated -Z");
    EXPECT_NEAR(camera.GetViewMatrix().TransformPoint(Vector3(0.0f, 0.0f, 0.0f)).z, -5.0f, Tolerance);
    ExpectTargetCentredInFront(camera, Vector3(0.0f, 0.0f, 0.0f), "from +Z");
    ExpectNear(camera.GetUp(), Vector3(0.0f, 1.0f, 0.0f), "GetUp");
    ExpectNear(camera.GetRight(), Vector3(1.0f, 0.0f, 0.0f), "GetRight");
}

TEST(CameraLookAtTest, TargetProjectsToTheCentreFromAnyPose)
{
    struct Pose
    {
        Vector3 position;
        Vector3 target;
        const char *name;
    };
    const Pose poses[] = {
        {{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, "from +Z"},
        {{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 0.0f}, "from -Z"},
        {{7.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, "from +X"},
        {{0.0f, 8.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, "from above"},
        {{0.0f, -6.0f, -2.0f}, {0.0f, 0.0f, 0.0f}, "from below"},
        {{5.0f, 5.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, "diagonal"},
        {{5.0f, -2.0f, 8.0f}, {1.0f, 2.0f, 3.0f}, "offset target"},
        {{-3.0f, 2.0f, -7.0f}, {1.0f, -1.0f, 2.0f}, "offset diagonal"},
    };

    for (const Pose &pose : poses)
    {
        Camera camera = PerspectiveCamera(pose.position);
        camera.LookAt(pose.target);
        ExpectTargetCentredInFront(camera, pose.target, pose.name);
    }
}

TEST(CameraLookAtTest, UpVectorIsHonoured)
{
    // Default up: a point above the target is above the centre, a point to the camera's right is right of it
    {
        Camera camera = PerspectiveCamera(Vector3(0.0f, 0.0f, 5.0f));
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f));
        const Vector3 above = camera.GetViewProjectionMatrix().TransformPoint(Vector3(0.0f, 1.0f, 0.0f));
        EXPECT_GT(above.y, 0.0f);
        EXPECT_NEAR(above.x, 0.0f, Tolerance);
        const Vector3 right = camera.GetViewProjectionMatrix().TransformPoint(Vector3(1.0f, 0.0f, 0.0f));
        EXPECT_GT(right.x, 0.0f) << "world +X is screen right from +Z";
        EXPECT_NEAR(right.y, 0.0f, Tolerance);
    }

    // A diagonal pose: the target plus the up vector is straight above the centre
    {
        const Vector3 target(1.0f, 2.0f, 3.0f);
        Camera camera = PerspectiveCamera(Vector3(6.0f, 5.0f, -4.0f));
        camera.LookAt(target);
        const Vector3 above = camera.GetViewProjectionMatrix().TransformPoint(target + Vector3(0.0f, 1.0f, 0.0f));
        EXPECT_GT(above.y, 0.0f);
        EXPECT_NEAR(above.x, 0.0f, Tolerance);
        EXPECT_GT(camera.GetUp().y, 0.0f);
        const Vector3 right = camera.GetViewProjectionMatrix().TransformPoint(target + camera.GetRight());
        EXPECT_GT(right.x, 0.0f);
        EXPECT_NEAR(right.y, 0.0f, Tolerance);
    }

    // A custom up (+X): world +X is now the top of the screen
    {
        Camera camera = PerspectiveCamera(Vector3(0.0f, 0.0f, 5.0f));
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        ExpectTargetCentredInFront(camera, Vector3(0.0f, 0.0f, 0.0f), "up +X");
        const Vector3 above = camera.GetViewProjectionMatrix().TransformPoint(Vector3(1.0f, 0.0f, 0.0f));
        EXPECT_GT(above.y, 0.0f);
        EXPECT_NEAR(above.x, 0.0f, Tolerance);
        ExpectNear(camera.GetUp(), Vector3(1.0f, 0.0f, 0.0f), "GetUp");
    }
}

TEST(CameraLookAtTest, ScreenCentreRayPassesThroughTheTarget)
{
    const Vector3 target(1.0f, 2.0f, 3.0f);

    Camera perspective = PerspectiveCamera(Vector3(5.0f, -2.0f, 8.0f));
    Camera orthographic;
    orthographic.SetOrthographic(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
    orthographic.SetPosition(Vector3(-6.0f, 7.0f, 2.0f));

    for (Camera *camera : {&perspective, &orthographic})
    {
        camera->LookAt(target);
        const Math::Ray ray = camera->ScreenPointToRay(Vector2(400.0f, 300.0f), Viewport);
        ExpectNear(ray.direction, camera->GetForward(), "centre ray direction is the view direction");

        const float along = (target - ray.origin).Dot(ray.direction);
        EXPECT_GT(along, 0.0f) << "the target is ahead of the near plane";
        ExpectNear(ray.GetPoint(along), target, "closest point on the centre ray");
    }
}

TEST(CameraLookAtTest, ScreenUpDoesNotFlipWhereTheUpFallbackStarts)
{
    // An orbit camera passing over the target from the +Z side: just outside the fallback threshold the
    // default up (+Y) is used, just inside it the world Z fallback is. The screen's up must be the same both
    // sides (world -Z, the side +Y projects to), not rolled 180 degrees.
    for (const float z : {0.01f, 0.006f, 0.005f, 0.001f})
    {
        Camera camera = PerspectiveCamera(Vector3(0.0f, 5.0f, z));
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f));
        EXPECT_LT(camera.GetUp().z, -0.9f) << "z = " << z;
    }
    // And from the -Z side, the mirror image
    for (const float z : {-0.01f, -0.006f, -0.005f, -0.001f})
    {
        Camera camera = PerspectiveCamera(Vector3(0.0f, 5.0f, z));
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f));
        EXPECT_GT(camera.GetUp().z, 0.9f) << "z = " << z;
    }
}

TEST(CameraLookAtTest, DegenerateCasesStayFinite)
{
    // Looking at its own position: no direction, so the rotation is kept
    {
        Camera camera = PerspectiveCamera(Vector3(1.0f, 2.0f, 3.0f));
        const Math::Quaternion turned = Math::Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.5f);
        camera.SetRotation(turned);
        camera.LookAt(Vector3(1.0f, 2.0f, 3.0f));
        EXPECT_TRUE(IsFinite(camera.GetRotation()));
        EXPECT_NEAR(camera.GetRotation().w, turned.w, Tolerance);
        EXPECT_NEAR(camera.GetRotation().y, turned.y, Tolerance);
        EXPECT_TRUE(IsFinite(camera.GetViewMatrix()));
    }

    // Up parallel to the view direction (straight down and straight up), and a zero up vector: a fallback up
    // is used, and the target is still centred in front
    struct Case
    {
        Vector3 position;
        Vector3 up;
        const char *name;
    };
    const Case cases[] = {
        {{0.0f, 10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, "straight down, up +Y"},
        {{0.0f, -10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, "straight up, up +Y"},
        {{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}, "up along the view axis"},
        {{3.0f, 4.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, "zero up"},
    };
    for (const Case &c : cases)
    {
        Camera camera = PerspectiveCamera(c.position);
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f), c.up);
        EXPECT_TRUE(IsFinite(camera.GetRotation())) << c.name;
        EXPECT_TRUE(IsFinite(camera.GetViewMatrix())) << c.name;
        EXPECT_TRUE(IsFinite(camera.GetViewProjectionMatrix())) << c.name;
        ExpectTargetCentredInFront(camera, Vector3(0.0f, 0.0f, 0.0f), c.name);
    }
}
