#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <editor-server/EditorCamera.hpp>
#include <editor-server/FrameTracker.hpp>
#include <engine/Camera.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;

// The editor camera's maths (#79, E7a): the Camera it builds, and the checks on a camera a client sent; and the
// frame revision bookkeeping behind render on demand. No server, window or renderer is involved.

namespace
{
    using Vec4 = std::array<float, 4>;

    /// matrix * (x, y, z, w) for a column vector (the engine's convention: the translation is column 3)
    Vec4 Apply(const Matrix4 &m, const float x, const float y, const float z, const float w = 1.0f)
    {
        const float v[4] = {x, y, z, w};
        Vec4 out{};
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t col = 0; col < 4; ++col)
            {
                out[row] += m(row, col) * v[col];
            }
        }
        return out;
    }

    /// The normalised device coordinates of a world point
    Vec4 Ndc(const Camera &camera, const float x, const float y, const float z)
    {
        const Vec4 view = Apply(camera.GetViewMatrix(), x, y, z);
        const Vec4 clip = Apply(camera.GetProjectionMatrix(), view[0], view[1], view[2], view[3]);
        return {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], clip[3]};
    }

    constexpr float Nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float Inf = std::numeric_limits<float>::infinity();
}

// ==================== The camera it builds ====================

TEST(EditorCameraTest, ThePerspectiveCameraLooksDownMinusZFromItsPosition)
{
    EditorCameraState state;
    state.position = Math::Vector3(0.0f, 0.0f, 10.0f);
    state.fovY = 90.0f;
    const Camera camera = state.ToCamera(2.0f);

    // The origin is straight ahead, 10 units out: the centre of the screen, with w = distance
    const Vec4 origin = Ndc(camera, 0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(origin[0], 0.0f, 1e-5f);
    EXPECT_NEAR(origin[1], 0.0f, 1e-5f);
    EXPECT_NEAR(origin[3], 10.0f, 1e-4f);

    // A 90 degree vertical field of view sees +-10 up and down at that distance, and +-20 across at aspect 2
    EXPECT_NEAR(Ndc(camera, 0.0f, 5.0f, 0.0f)[1], 0.5f, 1e-3f);
    EXPECT_NEAR(Ndc(camera, 10.0f, 0.0f, 0.0f)[0], 0.5f, 1e-3f);
    EXPECT_NEAR(Ndc(camera, 20.0f, 10.0f, 0.0f)[0], 1.0f, 1e-3f);
    EXPECT_NEAR(Ndc(camera, 20.0f, 10.0f, 0.0f)[1], 1.0f, 1e-3f);

    // Behind the camera: negative w
    EXPECT_LT(Ndc(camera, 0.0f, 0.0f, 20.0f)[3], 0.0f);
    EXPECT_EQ(camera.GetPosition().z, 10.0f);
}

TEST(EditorCameraTest, TheAspectRatioIsTheViewports)
{
    EditorCameraState state;
    state.fovY = 90.0f;
    state.position = Math::Vector3(0.0f, 0.0f, 10.0f);
    EXPECT_NEAR(Ndc(state.ToCamera(1.0f), 10.0f, 0.0f, 0.0f)[0], 1.0f, 1e-3f);
    EXPECT_NEAR(Ndc(state.ToCamera(4.0f), 10.0f, 0.0f, 0.0f)[0], 0.25f, 1e-3f);

    // A viewport with no sensible aspect ratio never gives a NaN projection
    for (const float aspect : {0.0f, -1.0f, Nan, Inf})
    {
        const Camera camera = state.ToCamera(aspect);
        EXPECT_NEAR(Ndc(camera, 10.0f, 0.0f, 0.0f)[0], 1.0f, 1e-3f) << aspect;
    }
}

TEST(EditorCameraTest, TheOrthographicCameraSpansOrthoSizeAboveAndBelow)
{
    EditorCameraState state;
    state.orthographic = true;
    state.orthoSize = 4.0f;
    state.position = Math::Vector3(1.0f, 2.0f, 10.0f);
    const Camera camera = state.ToCamera(2.0f);

    // Half height 4 and half width 8, around the camera's own position; w stays 1 (no perspective)
    const Vec4 centre = Ndc(camera, 1.0f, 2.0f, 0.0f);
    EXPECT_NEAR(centre[0], 0.0f, 1e-5f);
    EXPECT_NEAR(centre[1], 0.0f, 1e-5f);
    EXPECT_NEAR(centre[3], 1.0f, 1e-6f);
    EXPECT_NEAR(Ndc(camera, 1.0f, 6.0f, 0.0f)[1], 1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(camera, 9.0f, 2.0f, 0.0f)[0], 1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(camera, -7.0f, -2.0f, 0.0f)[0], -1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(camera, -7.0f, -2.0f, 0.0f)[1], -1.0f, 1e-5f);

    // Distance doesn't shrink anything
    EXPECT_NEAR(Ndc(camera, 9.0f, 2.0f, -50.0f)[0], 1.0f, 1e-5f);
}

TEST(EditorCameraTest, TheRotationTurnsTheView)
{
    EditorCameraState state;
    state.position = Math::Vector3(0.0f, 0.0f, 0.0f);
    // A half turn about Y: the camera that looked down -Z now looks down +Z
    state.rotation = Math::Quaternion(0.0f, 0.0f, 1.0f, 0.0f); // w, x, y, z
    const Camera camera = state.ToCamera(1.0f);

    EXPECT_NEAR(camera.GetForward().z, 1.0f, 1e-5f);
    EXPECT_GT(Ndc(camera, 0.0f, 0.0f, 5.0f)[3], 0.0f) << "a point along +Z is in front";
    EXPECT_LT(Ndc(camera, 0.0f, 0.0f, -5.0f)[3], 0.0f) << "and one along -Z is behind";
    EXPECT_NEAR(Ndc(camera, 0.0f, 0.0f, 5.0f)[0], 0.0f, 1e-5f);
}

TEST(EditorCameraTest, TwoCamerasWithTheSameStateAreEqual)
{
    EditorCameraState a;
    EditorCameraState b;
    EXPECT_TRUE(a == b);
    b.farPlane = 999.0f;
    EXPECT_FALSE(a == b);
    b = a;
    b.position = Math::Vector3(0.0f, 0.0f, 10.5f);
    EXPECT_FALSE(a == b);
    b = a;
    b.orthographic = true;
    EXPECT_FALSE(a == b);
}

// ==================== The checks on a camera a client sent ====================

TEST(EditorCameraCheckTest, TheDefaultsAndOrdinaryCamerasPass)
{
    EXPECT_TRUE(CheckEditorCamera(EditorCameraState{}).state.has_value());

    EditorCameraState wide;
    wide.position = Math::Vector3(-999999.0f, 5.0f, 999999.0f);
    wide.nearPlane = 0.01f;
    wide.farPlane = 5.0e4f;
    wide.fovY = 1.0f;
    EXPECT_TRUE(CheckEditorCamera(wide).state.has_value());
    wide.fovY = 179.0f;
    EXPECT_TRUE(CheckEditorCamera(wide).state.has_value());

    EditorCameraState ortho;
    ortho.orthographic = true;
    ortho.orthoSize = 0.001f;
    ortho.nearPlane = -100.0f;
    ortho.farPlane = 100.0f;
    EXPECT_TRUE(CheckEditorCamera(ortho).state.has_value());
}

TEST(EditorCameraCheckTest, ANonFiniteNumberIsRefusedWhicheverFieldItIsIn)
{
    for (const float bad : {Nan, Inf, -Inf})
    {
        for (int field = 0; field < 9; ++field)
        {
            EditorCameraState state;
            switch (field)
            {
            case 0: state.position = Math::Vector3(bad, 0.0f, 0.0f); break;
            case 1: state.position = Math::Vector3(0.0f, bad, 0.0f); break;
            case 2: state.position = Math::Vector3(0.0f, 0.0f, bad); break;
            case 3: state.rotation = Math::Quaternion(bad, 0.0f, 0.0f, 0.0f); break;
            case 4: state.rotation = Math::Quaternion(1.0f, bad, 0.0f, 0.0f); break;
            case 5: state.rotation = Math::Quaternion(1.0f, 0.0f, 0.0f, bad); break;
            case 6: state.fovY = bad; break;
            case 7: state.orthoSize = bad; break;
            default: state.nearPlane = bad; break;
            }
            const EditorCameraCheck result = CheckEditorCamera(state);
            EXPECT_FALSE(result.state.has_value()) << "field " << field << " = " << bad;
            EXPECT_FALSE(result.error.empty()) << "field " << field;
        }
        EditorCameraState state;
        state.farPlane = bad;
        EXPECT_FALSE(CheckEditorCamera(state).state.has_value()) << "far = " << bad;
    }
}

TEST(EditorCameraCheckTest, ValuesBeyondTheLimitsAreRefusedNotClamped)
{
    const auto refused = [](auto change)
    {
        EditorCameraState state;
        change(state);
        return !CheckEditorCamera(state).state.has_value();
    };
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.position = Math::Vector3(1.0e7f, 0.0f, 0.0f); }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.position = Math::Vector3(0.0f, -3.0e38f, 0.0f); }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.fovY = 0.5f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.fovY = 179.5f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.nearPlane = 0.0f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.nearPlane = 1.0e-5f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.nearPlane = 2.0e6f; s.farPlane = 5.0e6f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.farPlane = s.nearPlane + 1.0e-4f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.farPlane = s.nearPlane; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.farPlane = 2.0e7f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.orthographic = true; s.orthoSize = 0.0f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.orthographic = true; s.orthoSize = 2.0e6f; }));
    EXPECT_TRUE(refused([](EditorCameraState &s) { s.orthographic = true; s.nearPlane = -2.0e6f; }));

    // An orthographic camera doesn't care about a field of view it doesn't use (as long as it is a number), nor a
    // perspective one about an orthoSize
    EXPECT_FALSE(refused([](EditorCameraState &s) { s.orthographic = true; s.fovY = 0.0f; }));
    EXPECT_FALSE(refused([](EditorCameraState &s) { s.orthoSize = 0.0f; }));
}

TEST(EditorCameraCheckTest, TheRotationIsNormalisedAndAZeroOneIsRefused)
{
    EditorCameraState state;
    state.rotation = Math::Quaternion(2.0f, 0.0f, 0.0f, 0.0f); // w = 2
    const EditorCameraCheck normalised = CheckEditorCamera(state);
    ASSERT_TRUE(normalised.state.has_value());
    EXPECT_NEAR(normalised.state->rotation.GetW(), 1.0f, 1e-6f);
    EXPECT_NEAR(normalised.state->rotation.Length(), 1.0f, 1e-6f);

    state.rotation = Math::Quaternion(1.0f, 1.0f, 1.0f, 1.0f);
    const EditorCameraCheck diagonal = CheckEditorCamera(state);
    ASSERT_TRUE(diagonal.state.has_value());
    EXPECT_NEAR(diagonal.state->rotation.Length(), 1.0f, 1e-6f);
    EXPECT_NEAR(diagonal.state->rotation.GetX(), 0.5f, 1e-6f);

    state.rotation = Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(CheckEditorCamera(state).state.has_value());
    state.rotation = Math::Quaternion(1.0e-8f, 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(CheckEditorCamera(state).state.has_value()) << "too small to have a direction";
    // Squaring would overflow float: Length() is inf, so it is refused instead of normalised to NaN
    state.rotation = Math::Quaternion(3.0e38f, 3.0e38f, 0.0f, 0.0f);
    EXPECT_FALSE(CheckEditorCamera(state).state.has_value());
}

TEST(EditorCameraCheckTest, APerspectiveDepthRangeIsLimitedByTheRatioOfFarToNear)
{
    EditorCameraState state;
    state.nearPlane = 0.001f;
    state.farPlane = 5000.0f; // ratio 5e6
    EXPECT_TRUE(CheckEditorCamera(state).state.has_value());
    state.farPlane = 50000.0f; // ratio 5e7
    const EditorCameraCheck refused = CheckEditorCamera(state);
    EXPECT_FALSE(refused.state.has_value());
    EXPECT_NE(refused.error.find("far"), std::string::npos) << refused.error;

    // An orthographic camera has no such ratio
    state.orthographic = true;
    EXPECT_TRUE(CheckEditorCamera(state).state.has_value());
}

TEST(EditorCameraCheckTest, TheErrorNamesTheFieldThatWasRefused)
{
    const auto errorFor = [](auto change)
    {
        EditorCameraState state;
        change(state);
        return CheckEditorCamera(state).error;
    };
    const auto mentions = [](const std::string &error, const char *word) { return error.find(word) != std::string::npos; };
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.fovY = Nan; }), "field of view"));
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.orthoSize = Inf; }), "orthographic size"));
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.nearPlane = Nan; }), "near plane"));
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.farPlane = Inf; }), "far plane"));
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.position = Math::Vector3(Nan, 0.0f, 0.0f); }), "position"));
    EXPECT_TRUE(mentions(errorFor([](EditorCameraState &s) { s.rotation = Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f); }), "rotation"));
}

TEST(EditorCameraTest, TheProjectionThatIsntInUseIsNotPartOfEquality)
{
    EditorCameraState a;
    EditorCameraState b = a;
    b.orthoSize = 99.0f; // perspective: unused
    EXPECT_TRUE(a == b);
    b.fovY = 70.0f;
    EXPECT_FALSE(a == b);

    a.orthographic = true;
    b = a;
    b.fovY = 120.0f; // orthographic: unused
    EXPECT_TRUE(a == b);
    b.orthoSize = 6.0f;
    EXPECT_FALSE(a == b);
}

// ==================== The frame revision ====================

TEST(FrameTrackerTest, ARevisionIsNeverZeroAndOnlyGrows)
{
    FrameTracker tracker;
    EXPECT_EQ(tracker.Revision(), 1u);
    EXPECT_EQ(FrameTracker(0).Revision(), 1u) << "0 is made 1: a client passes 0 for no frame";
    EXPECT_EQ(FrameTracker(77).Revision(), 77u);

    tracker.MarkChanged();
    EXPECT_EQ(tracker.Revision(), 2u);
    tracker.MarkChanged();
    EXPECT_EQ(tracker.Revision(), 3u);

    // Past the largest, it goes to 1, skipping 0
    FrameTracker last(0xFFFFFFFFu);
    EXPECT_EQ(last.Revision(), 0xFFFFFFFFu);
    last.MarkChanged();
    EXPECT_EQ(last.Revision(), 1u);
}

TEST(FrameTrackerTest, OnlyAFrameRenderedAtTheCurrentRevisionIsCurrent)
{
    FrameTracker tracker(5);
    // Nothing rendered yet: even the number a client holds from another host is not current
    EXPECT_FALSE(tracker.IsCurrent(5));
    EXPECT_FALSE(tracker.IsCurrent(0));
    EXPECT_FALSE(tracker.HasCurrentFrame());

    tracker.MarkRendered();
    EXPECT_TRUE(tracker.IsCurrent(5));
    EXPECT_TRUE(tracker.HasCurrentFrame());
    EXPECT_FALSE(tracker.IsCurrent(0)) << "0 means no frame, and always gets one";
    EXPECT_FALSE(tracker.IsCurrent(4));
    EXPECT_FALSE(tracker.IsCurrent(6));

    tracker.MarkChanged();
    EXPECT_FALSE(tracker.IsCurrent(5)) << "the picture moved on";
    EXPECT_FALSE(tracker.IsCurrent(6)) << "and nothing was rendered at 6";
    EXPECT_FALSE(tracker.HasCurrentFrame());

    tracker.MarkRendered();
    EXPECT_TRUE(tracker.IsCurrent(6));
    EXPECT_FALSE(tracker.IsCurrent(5));
}

TEST(FrameTrackerTest, ABufferUsedByAnotherCommandNeedsARenderButChangesNoRevision)
{
    FrameTracker tracker;
    tracker.MarkRendered();
    const uint32_t revision = tracker.Revision();

    tracker.InvalidateBuffer();
    EXPECT_EQ(tracker.Revision(), revision);
    EXPECT_TRUE(tracker.IsCurrent(revision)) << "a client's own copy is still the current picture";
    EXPECT_FALSE(tracker.HasCurrentFrame()) << "but the server has to render it again to send one";

    tracker.MarkRendered();
    EXPECT_TRUE(tracker.HasCurrentFrame());
}

TEST(FrameTrackerTest, ChangesAreAnnouncedOncePerRenderedFrame)
{
    FrameTracker tracker;
    EXPECT_TRUE(tracker.MarkChanged()) << "the first change";
    EXPECT_FALSE(tracker.MarkChanged());
    EXPECT_FALSE(tracker.MarkChanged());

    tracker.MarkRendered();
    EXPECT_TRUE(tracker.MarkChanged()) << "a render re-arms it";
    EXPECT_FALSE(tracker.MarkChanged());

    // Using the buffer for something else isn't a render
    tracker.InvalidateBuffer();
    EXPECT_FALSE(tracker.MarkChanged());
}

TEST(FrameTrackerTest, ASceneChangeThatWasReportedIsNotCountedAgainWhenObserved)
{
    FrameTracker tracker;
    int scene = 0;
    (void)tracker.ObserveScene(&scene, 3);
    tracker.MarkRendered();
    const uint32_t rendered = tracker.Revision();

    // The server reports a change (one revision on) and records the scene it made it to
    tracker.MarkChanged();
    tracker.RecordScene(&scene, 4);
    EXPECT_FALSE(tracker.ObserveScene(&scene, 4));
    EXPECT_EQ(tracker.Revision(), rendered + 1);

    // A change nobody reported is still noticed
    EXPECT_FALSE(tracker.ObserveScene(&scene, 4));
    tracker.MarkRendered();
    EXPECT_TRUE(tracker.ObserveScene(&scene, 5));
    EXPECT_EQ(tracker.Revision(), rendered + 2);
}

TEST(FrameTrackerTest, AnAnnouncementThatWentUnansweredCanBeRearmed)
{
    FrameTracker tracker;
    EXPECT_TRUE(tracker.MarkChanged());
    EXPECT_FALSE(tracker.MarkChanged()) << "announced, and no frame since";
    tracker.RearmAnnouncement();
    EXPECT_TRUE(tracker.MarkChanged()) << "a render that failed doesn't leave it stuck";
}

TEST(FrameTrackerTest, ASceneChangeNobodyReportedIsNoticed)
{
    FrameTracker tracker;
    int sceneA = 0;
    int sceneB = 0;

    // The first look only records the scene
    EXPECT_FALSE(tracker.ObserveScene(&sceneA, 3));
    EXPECT_EQ(tracker.Revision(), 1u);
    tracker.MarkRendered();

    // The same scene at the same revision: no change, however often it is looked at
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FALSE(tracker.ObserveScene(&sceneA, 3));
    }
    EXPECT_EQ(tracker.Revision(), 1u);
    EXPECT_TRUE(tracker.IsCurrent(1));

    // Edited
    EXPECT_TRUE(tracker.ObserveScene(&sceneA, 4));
    EXPECT_EQ(tracker.Revision(), 2u);
    EXPECT_FALSE(tracker.ObserveScene(&sceneA, 4)) << "noticed once";
    EXPECT_EQ(tracker.Revision(), 2u);
    tracker.MarkRendered();

    // Another scene, even at the same revision number (or none at all)
    EXPECT_TRUE(tracker.ObserveScene(&sceneB, 4));
    EXPECT_EQ(tracker.Revision(), 3u);
    tracker.MarkRendered();
    EXPECT_TRUE(tracker.ObserveScene(nullptr, 4));
    EXPECT_EQ(tracker.Revision(), 4u);
}
