#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Camera.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using Math::Vector2;
using Math::Vector3;

// Picking without a window or physics backend: the screen-to-ray maths, the mouse's button edges through
// its InjectPointer seam, and the PointerDispatcher state machine driven by a fake hit provider

namespace
{
    constexpr float Tolerance = 1e-3f;

    void ExpectNear(const Vector3 &actual, const Vector3 &expected, const char *what)
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << ".x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << ".y";
        EXPECT_NEAR(actual.z, expected.z, Tolerance) << what << ".z";
    }

    const Vector2i Viewport{800, 600};
}

// ============================================================================
// Camera::ScreenPointToRay
// ============================================================================

TEST(ScreenPointToRayTest, PerspectiveCentreRayLooksAlongTheView)
{
    Camera camera;
    camera.SetPerspective(90.0f, 4.0f / 3.0f, 0.5f, 50.0f);
    camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));

    float nearToFar = 0.0f;
    const Math::Ray ray = camera.ScreenPointToRay(Vector2(400.0f, 300.0f), Viewport, &nearToFar);

    ExpectNear(ray.origin, Vector3(0.0f, 0.0f, 9.5f), "origin is on the near plane");
    ExpectNear(ray.direction, Vector3(0.0f, 0.0f, -1.0f), "direction");
    EXPECT_NEAR(nearToFar, 49.5f, 0.01f);
}

TEST(ScreenPointToRayTest, PerspectiveCornersFlipY)
{
    Camera camera;
    camera.SetPerspective(90.0f, 4.0f / 3.0f, 0.5f, 50.0f); // tan(45) = 1: corners are at (+-4/3, +-1, -1)
    camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));

    // The window's top-left is the view's top-left: y up in the world
    const Math::Ray topLeft = camera.ScreenPointToRay(Vector2(0.0f, 0.0f), Viewport);
    ExpectNear(topLeft.direction, Vector3(-4.0f / 3.0f, 1.0f, -1.0f).Normalized(), "top-left direction");
    ExpectNear(topLeft.origin, Vector3(-2.0f / 3.0f, 0.5f, 9.5f), "top-left origin");

    const Math::Ray bottomRight = camera.ScreenPointToRay(Vector2(800.0f, 600.0f), Viewport);
    ExpectNear(bottomRight.direction, Vector3(4.0f / 3.0f, -1.0f, -1.0f).Normalized(), "bottom-right direction");
}

TEST(ScreenPointToRayTest, OrthographicRaysAreParallel)
{
    Camera camera;
    camera.SetOrthographic(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
    camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));

    float nearToFar = 0.0f;
    const Math::Ray centre = camera.ScreenPointToRay(Vector2(400.0f, 300.0f), Viewport, &nearToFar);
    ExpectNear(centre.origin, Vector3(0.0f, 0.0f, 9.9f), "centre origin");
    ExpectNear(centre.direction, Vector3(0.0f, 0.0f, -1.0f), "centre direction");
    EXPECT_NEAR(nearToFar, 99.9f, 0.01f);

    const Math::Ray topLeft = camera.ScreenPointToRay(Vector2(0.0f, 0.0f), Viewport);
    ExpectNear(topLeft.origin, Vector3(-4.0f, 3.0f, 9.9f), "top-left origin");
    ExpectNear(topLeft.direction, Vector3(0.0f, 0.0f, -1.0f), "top-left direction");

    const Math::Ray bottomRight = camera.ScreenPointToRay(Vector2(800.0f, 600.0f), Viewport);
    ExpectNear(bottomRight.origin, Vector3(4.0f, -3.0f, 9.9f), "bottom-right origin");
}

TEST(ScreenPointToRayTest, WideOrthographicProjectionWorks)
{
    // A pixel-sized 2D camera: its projection's determinant (~4e-9) is below what Matrix4::inverse accepts
    Camera camera;
    camera.SetOrthographic(-960.0f, 960.0f, -540.0f, 540.0f, 0.1f, 1000.0f);

    const Math::Ray topLeft = camera.ScreenPointToRay(Vector2(0.0f, 0.0f), Vector2i{1920, 1080});
    EXPECT_NEAR(topLeft.origin.x, -960.0f, 0.01f);
    EXPECT_NEAR(topLeft.origin.y, 540.0f, 0.01f);
    ExpectNear(topLeft.direction, Vector3(0.0f, 0.0f, -1.0f), "direction");
}

TEST(ScreenPointToRayTest, RotatedCameraRoundTripsThroughViewProjection)
{
    Camera camera;
    camera.SetPerspective(60.0f, 4.0f / 3.0f, 0.1f, 100.0f);
    camera.SetPosition(Vector3(5.0f, -2.0f, 8.0f));
    camera.LookAt(Vector3(1.0f, 2.0f, 3.0f));

    const Matrix4 viewProjection = camera.GetViewProjectionMatrix();
    for (const Vector2 screen : {Vector2(400.0f, 300.0f), Vector2(100.0f, 50.0f), Vector2(700.0f, 520.0f)})
    {
        const Math::Ray ray = camera.ScreenPointToRay(screen, Viewport);
        EXPECT_NEAR(ray.direction.Length(), 1.0f, Tolerance);

        const float expectedX = 2.0f * screen.x / 800.0f - 1.0f;
        const float expectedY = 1.0f - 2.0f * screen.y / 600.0f;
        for (const float distance : {0.0f, 5.0f, 40.0f})
        {
            const Vector3 ndc = viewProjection.TransformPoint(ray.GetPoint(distance));
            EXPECT_NEAR(ndc.x, expectedX, Tolerance) << "distance " << distance;
            EXPECT_NEAR(ndc.y, expectedY, Tolerance) << "distance " << distance;
        }
        EXPECT_NEAR(viewProjection.TransformPoint(ray.origin).z, -1.0f, Tolerance) << "starts on the near plane";
    }
}

TEST(ScreenPointToRayTest, EmptyViewportGivesTheCentreRay)
{
    Camera camera;
    camera.SetPerspective(60.0f, 1.0f, 0.1f, 100.0f);

    const Math::Ray ray = camera.ScreenPointToRay(Vector2(123.0f, 45.0f), Vector2i{0, 0});
    ExpectNear(ray.direction, Vector3(0.0f, 0.0f, -1.0f), "direction");
}

// ============================================================================
// Mouse buttons
// ============================================================================

TEST(MouseButtonTest, EdgesLastOneUpdate)
{
    Mouse mouse(nullptr);
    EXPECT_FALSE(mouse.GetButton(0));

    mouse.InjectPointer(Vector2(10.0f, 20.0f), Mouse::ButtonBit(0));
    mouse.Update();
    EXPECT_TRUE(mouse.GetButton(0));
    EXPECT_TRUE(mouse.GetButtonDown(0));
    EXPECT_FALSE(mouse.GetButtonUp(0));
    EXPECT_FLOAT_EQ(mouse.GetPosition().x, 10.0f);
    EXPECT_FLOAT_EQ(mouse.GetPosition().y, 20.0f);

    mouse.Update(); // no window and nothing injected: the state is held, so the edge is gone
    EXPECT_TRUE(mouse.GetButton(0));
    EXPECT_FALSE(mouse.GetButtonDown(0));
    EXPECT_FLOAT_EQ(mouse.GetPosition().x, 10.0f);

    mouse.InjectPointer(Vector2(10.0f, 20.0f), 0);
    mouse.Update();
    EXPECT_FALSE(mouse.GetButton(0));
    EXPECT_TRUE(mouse.GetButtonUp(0));

    mouse.Update();
    EXPECT_FALSE(mouse.GetButtonUp(0));
}

TEST(MouseButtonTest, ButtonsAreIndependentAndOutOfRangeReadsFalse)
{
    Mouse mouse(nullptr);
    mouse.InjectPointer(Vector2(0.0f, 0.0f), Mouse::ButtonBit(1) | Mouse::ButtonBit(7));
    mouse.Update();

    EXPECT_FALSE(mouse.GetButton(0));
    EXPECT_TRUE(mouse.GetButtonDown(1));
    EXPECT_TRUE(mouse.GetButton(MouseButton::Right));
    EXPECT_TRUE(mouse.GetButton(7));
    EXPECT_FALSE(mouse.GetButton(8));
    EXPECT_FALSE(mouse.GetButton(-1));
    EXPECT_EQ(Mouse::ButtonBit(8), 0u);
}

TEST(MouseButtonTest, InjectedMoveSetsTheDelta)
{
    Mouse mouse(nullptr);
    mouse.InjectPointer(Vector2(5.0f, 5.0f), 0);
    mouse.Update();
    mouse.InjectPointer(Vector2(8.0f, 1.0f), 0);
    mouse.Update();
    EXPECT_FLOAT_EQ(mouse.GetPositionDelta().x, 3.0f);
    EXPECT_FLOAT_EQ(mouse.GetPositionDelta().y, -4.0f);
}

// ============================================================================
// PointerDispatcher
// ============================================================================

namespace
{
    // Logs every pointer event as "<name>.<event>"; a hook can act from inside a callback
    class PointerRecorder final : public Component
    {
    public:
        explicit PointerRecorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "PointerRecorder"; }

        void OnMouseEnter() override { Record("Enter"); }
        void OnMouseOver() override { Record("Over"); }
        void OnMouseExit() override { Record("Exit"); }
        void OnMouseDown() override { Record("Down"); }
        void OnMouseDrag() override { Record("Drag"); }
        void OnMouseUp() override { Record("Up"); }
        void OnMouseUpAsButton() override { Record("UpAsButton"); }

        std::string name;
        std::vector<std::string> *log = nullptr;
        std::function<void(const std::string &)> hook;

    private:
        void Record(const std::string &event)
        {
            log->push_back(name + "." + event);
            if (hook)
            {
                hook(event);
            }
        }
    };
}

// The world is a fake: the provider returns whatever _under points at. Objects are not in a scene, so Destroy
// marks them at once (in a scene that happens when the destroy queue runs at the end of the frame).
class PointerDispatcherTest : public ::testing::Test
{
protected:
    PointerDispatcher _dispatcher;
    Mouse _mouse{nullptr};
    GameObject *_under = nullptr;
    std::vector<std::string> _log;

    void SetUp() override
    {
        _dispatcher.SetWorldHitProvider([this](const Vector2 &) { return _under; });
    }

    GameObject::Ptr Make(const std::string &name)
    {
        auto go = GameObject::Create(name);
        auto *recorder = go->AddComponent<PointerRecorder>();
        recorder->name = name;
        recorder->log = &_log;
        return go;
    }

    // One frame with the pointer over `under` and the left button held or not; returns that frame's events
    std::vector<std::string> Frame(const GameObject::Ptr &under, const bool held)
    {
        _log.clear();
        _under = under.get();
        _mouse.InjectPointer(Vector2(10.0f, 10.0f), held ? Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(PointerState::FromMouse(_mouse));
        return _log;
    }

    using Events = std::vector<std::string>;
};

TEST_F(PointerDispatcherTest, HoverSendsEnterOverAndExit)
{
    const auto a = Make("A");
    const auto b = Make("B");

    EXPECT_EQ(Frame(a, false), (Events{"A.Enter", "A.Over"}));
    EXPECT_EQ(Frame(a, false), (Events{"A.Over"}));
    EXPECT_EQ(_dispatcher.GetHovered(), a.get());
    EXPECT_EQ(Frame(b, false), (Events{"A.Exit", "B.Enter", "B.Over"}));
    EXPECT_EQ(Frame(nullptr, false), (Events{"B.Exit"}));
    EXPECT_EQ(Frame(nullptr, false), (Events{}));
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
}

TEST_F(PointerDispatcherTest, DownCapturesAndDragFollowsOffTheObject)
{
    const auto a = Make("A");
    const auto b = Make("B");

    EXPECT_EQ(Frame(a, true), (Events{"A.Down", "A.Enter", "A.Over"}));
    EXPECT_EQ(_dispatcher.GetCaptured(), a.get());
    EXPECT_EQ(Frame(nullptr, true), (Events{"A.Drag", "A.Exit"})) << "Drag continues off the object";
    EXPECT_EQ(Frame(b, true), (Events{"A.Drag", "B.Enter", "B.Over"})) << "B gets no Down or Drag";
    EXPECT_EQ(Frame(b, false), (Events{"A.Up", "B.Over"})) << "Up goes to the Down object; no UpAsButton off it";
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
}

TEST_F(PointerDispatcherTest, UpAsButtonOnlyWhenReleasedOverTheSameObject)
{
    const auto a = Make("A");

    EXPECT_EQ(Frame(a, true), (Events{"A.Down", "A.Enter", "A.Over"}));
    EXPECT_EQ(Frame(a, true), (Events{"A.Drag", "A.Over"}));
    EXPECT_EQ(Frame(a, false), (Events{"A.UpAsButton", "A.Up", "A.Over"}));

    // Pressed over it, dragged off and back before release: still a click on it, as in Unity
    Frame(a, true);
    Frame(nullptr, true);
    EXPECT_EQ(Frame(a, false), (Events{"A.UpAsButton", "A.Up", "A.Enter", "A.Over"}));
}

TEST_F(PointerDispatcherTest, PressOverNothingCapturesNothing)
{
    const auto a = Make("A");

    EXPECT_EQ(Frame(nullptr, true), (Events{}));
    EXPECT_EQ(Frame(a, true), (Events{"A.Enter", "A.Over"})) << "moving onto an object with the button held isn't a Down";
    EXPECT_EQ(Frame(a, false), (Events{"A.Over"}));
}

TEST_F(PointerDispatcherTest, DestroyedMidDragIsDroppedWithoutExitOrUp)
{
    const auto a = Make("A");

    Frame(a, true);
    a->Destroy();
    EXPECT_EQ(Frame(a, true), (Events{})) << "neither Drag, Over nor Exit";
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
    EXPECT_EQ(Frame(nullptr, false), (Events{})) << "no Up";
}

TEST_F(PointerDispatcherTest, DeactivatedWhileHoveredGetsNoExitAndReentersWhenActive)
{
    const auto a = Make("A");

    Frame(a, false);
    a->SetActive(false);
    EXPECT_EQ(Frame(a, false), (Events{})) << "an inactive object gets nothing, Exit included";
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);

    a->SetActive(true);
    EXPECT_EQ(Frame(a, false), (Events{"A.Enter", "A.Over"}));
}

TEST_F(PointerDispatcherTest, InactiveObjectGetsNothing)
{
    const auto parent = GameObject::Create("Parent");
    const auto a = Make("A");
    parent->AddChild(a);
    parent->SetActive(false); // inactive in the hierarchy, though active itself

    EXPECT_EQ(Frame(a, true), (Events{}));
    EXPECT_EQ(Frame(a, false), (Events{}));
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
}

TEST_F(PointerDispatcherTest, DisabledComponentStillReceives)
{
    const auto a = Make("A");
    a->GetComponent<PointerRecorder>()->SetActive(false);

    EXPECT_EQ(Frame(a, true), (Events{"A.Down", "A.Enter", "A.Over"})) << "the physics rule: only the object's state counts";
}

TEST_F(PointerDispatcherTest, EveryComponentReceivesAndDestroyingStopsDelivery)
{
    const auto a = Make("A");
    auto *second = a->AddComponent<PointerRecorder>();
    second->name = "A2";
    second->log = &_log;

    EXPECT_EQ(Frame(a, false), (Events{"A.Enter", "A2.Enter", "A.Over", "A2.Over"}));

    // The first component destroys its object from OnMouseDown: the second gets nothing more
    a->GetComponent<PointerRecorder>()->hook = [&a](const std::string &event)
    {
        if (event == "Down")
        {
            a->Destroy();
        }
    };
    EXPECT_EQ(Frame(a, true), (Events{"A.Down"}));
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
}

TEST_F(PointerDispatcherTest, UIProviderWinsOverTheWorld)
{
    const auto world = Make("World");
    const auto ui = Make("UI");
    GameObject *uiUnder = ui.get();
    _dispatcher.SetUIHitProvider([&uiUnder](const Vector2 &) { return uiUnder; });

    EXPECT_FALSE(_dispatcher.IsPointerOverUI());
    EXPECT_EQ(Frame(world, true), (Events{"UI.Down", "UI.Enter", "UI.Over"}));
    EXPECT_TRUE(_dispatcher.IsPointerOverUI());

    uiUnder = nullptr;
    EXPECT_EQ(Frame(world, false), (Events{"UI.Up", "UI.Exit", "World.Enter", "World.Over"}));
    EXPECT_FALSE(_dispatcher.IsPointerOverUI());
}

TEST_F(PointerDispatcherTest, MissedReleaseEndsTheOldCaptureBeforeANewDown)
{
    const auto a = Make("A");
    const auto b = Make("B");

    Frame(a, true);
    // A press that arrives while A still holds the capture (frames went unprocessed): A gets its Up first
    _log.clear();
    _under = b.get();
    _dispatcher.Process(PointerState{Vector2(1.0f, 1.0f), true, true, false});
    EXPECT_EQ(_log, (Events{"A.Up", "B.Down", "A.Exit", "B.Enter", "B.Over"}));
    EXPECT_EQ(_dispatcher.GetCaptured(), b.get());
}

TEST_F(PointerDispatcherTest, ReentrantProcessIsIgnored)
{
    const auto a = Make("A");
    a->GetComponent<PointerRecorder>()->hook = [this](const std::string &event)
    {
        if (event == "Down")
        {
            _dispatcher.Process(PointerState{Vector2(10.0f, 10.0f), true, true, false});
        }
    };

    EXPECT_EQ(Frame(a, true), (Events{"A.Down", "A.Enter", "A.Over"})) << "the nested Process sent nothing";
    EXPECT_EQ(Frame(a, true), (Events{"A.Drag", "A.Over"}));
}

TEST_F(PointerDispatcherTest, ResetFromACallbackStopsTheFrame)
{
    const auto a = Make("A");
    a->GetComponent<PointerRecorder>()->hook = [this](const std::string &event)
    {
        if (event == "Down")
        {
            _dispatcher.Reset();
        }
    };

    EXPECT_EQ(Frame(a, true), (Events{"A.Down"})) << "no hover callbacks after the Reset";
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);

    a->GetComponent<PointerRecorder>()->hook = nullptr;
    EXPECT_EQ(Frame(a, true), (Events{"A.Enter", "A.Over"})) << "forgotten: no Drag, entered afresh";
}

TEST_F(PointerDispatcherTest, ProviderReplacingItselfIsSafe)
{
    const auto a = Make("A");
    // The provider destroys itself while it runs; the dispatcher calls it through a copy
    _dispatcher.SetWorldHitProvider([this, target = a.get()](const Vector2 &)
    {
        _dispatcher.SetWorldHitProvider([](const Vector2 &) -> GameObject * { return nullptr; });
        return target;
    });

    EXPECT_EQ(Frame(a, false), (Events{"A.Enter", "A.Over"}));
    EXPECT_EQ(Frame(a, false), (Events{"A.Exit"})) << "the replacement is used from the next frame";
}

TEST_F(PointerDispatcherTest, ResetForgetsWithoutCallbacks)
{
    const auto a = Make("A");
    Frame(a, true);

    _dispatcher.Reset();
    EXPECT_EQ(_dispatcher.GetHovered(), nullptr);
    EXPECT_EQ(_dispatcher.GetCaptured(), nullptr);
    EXPECT_EQ(Frame(a, true), (Events{"A.Enter", "A.Over"}));
}

TEST_F(PointerDispatcherTest, UpdateWithoutAWindowDoesNothing)
{
    const auto a = Make("A");
    _under = a.get();
    _dispatcher.Update(); // Mouse::Get() is null: no application window in the tests
    EXPECT_TRUE(_log.empty());
}

TEST_F(PointerDispatcherTest, DefaultPickHitsNothingWithoutPhysics)
{
    // The world provider restored: the main camera's physics pick. Without a window or camera it finds nothing.
    _dispatcher.SetWorldHitProvider(nullptr);
    const auto a = Make("A");
    EXPECT_EQ(Frame(a, true), (Events{}));

    Camera camera;
    EXPECT_EQ(PointerDispatcher::PickWorld(camera, Vector2(1.0f, 1.0f), Vector2i{0, 0}, Layers::AllLayers), nullptr);
}
