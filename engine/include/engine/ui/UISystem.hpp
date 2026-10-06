#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Ray.hpp>
#include <math/Vector2.hpp>
#include <math/VectorN.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/input/PointerDispatcher.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine
{
    class Camera;
    class GameObject;
    class Scene;
}

namespace N2Engine::UI
{
    class UIGraphic;

    /// One graphic of the UI pass and the rect it draws over (canvas space)
    struct UIDrawItem
    {
        UIGraphic *graphic = nullptr;
        Rect rect{};
    };

    /// Where a ray crosses a world-space canvas's plane
    struct CanvasRayHit
    {
        /// Along the ray, in units of its direction's length (world units for a unit direction)
        float distance = 0.0f;
        /// The crossing point in canvas space (canvas units, y up, origin at the canvas rect's bottom-left)
        Math::Vector2 canvasPoint{0.0f, 0.0f};
    };

    /// A world-space canvas element under a ray: the object of the topmost raycast-target graphic there, its
    /// canvas, and where the ray crosses that canvas. gameObject is nullptr for no hit.
    struct WorldUIHit
    {
        GameObject *gameObject = nullptr;
        Canvas *canvas = nullptr;
        float distance = 0.0f;
        Math::Vector2 canvasPoint{0.0f, 0.0f};
    };

    /**
     * UI layout, drawing and hit testing, for screen-space overlay and world-space canvases. Stateless; everything is worked out from the
     * scene and the viewport each time, so it is deterministic and runs without a window or GPU.
     *
     * The viewport is the window size in window coordinates (Window::GetWindowDimensions, the cursor's space),
     * not the framebuffer, which is larger on high-DPI displays: the projection maps the canvas onto whatever
     * the framebuffer is.
     *
     * Layout and order (CollectGraphics):
     * 1. Canvases are the active Canvas components on objects active in the hierarchy with no Canvas above
     *    them. They are stable-sorted by sortOrder, ascending (ties keep hierarchy order: depth first, roots in
     *    scene order).
     * 2. Each canvas's rect is (0, 0, width, height). Its tree is walked depth first, parents before children,
     *    children in order, skipping inactive objects and their subtrees. An object's rect is its RectTransform
     *    resolved in its parent's rect (RectTransform::ResolveIn), or its parent's rect if it has none. The
     *    result is stored in the RectTransform (GetRect).
     * 3. Each object's active UIGraphic components are listed in component order.
     * So later items draw on top: a later canvas, a later sibling, a child over its parent.
     *
     * World-space canvases (Canvas::GetRenderMode() == WorldSpace) are left out of the overlay layout, the UI
     * pass and the overlay hit test. Each one draws as a single item of the scene's Transparent queue
     * (Canvas::RenderInQueue calls RenderWorldCanvas), laid out the same way in its own rect (0, 0, size), and
     * is hit by the camera's ray (HitTestWorldCanvases, HitTestScreenPoint).
     */
    class UISystem
    {
    public:
        /// Lays out every screen-space overlay canvas in the scene for the viewport and returns its graphics in
        /// draw order. Empty for an empty viewport. World-space canvases are skipped.
        [[nodiscard]] static std::vector<UIDrawItem> CollectGraphics(const Scene &scene, const Vector2i &viewport);

        /**
         * The UI pass, run by Application::Render after Scene::Render. Sets the renderer's view to identity and
         * its projection to OverlayProjection(viewport), then draws each item of CollectGraphics with a rect
         * of positive size, in order, with OverlayState(). Does nothing (and leaves the view and projection
         * alone) if CollectGraphics finds no graphics.
         */
        static void Render(const Scene &scene, Renderer::Common::IRenderer *renderer, const Vector2i &viewport);

        /// The topmost raycast-target graphic's object whose rect contains the point (canvas space: y up), or
        /// nullptr. Topmost is last in draw order; graphics with raycastTarget off are skipped.
        [[nodiscard]] static GameObject* HitTest(const Scene &scene, const Math::Vector2 &canvasPoint,
                                                 const Vector2i &viewport);
        /// HitTest for a point in window coordinates (top-left origin, y down; Mouse::GetPosition). A point
        /// outside the viewport (0 <= x < width, 0 <= y < height) hits nothing.
        [[nodiscard]] static GameObject* HitTestWindowPoint(const Scene &scene, const Math::Vector2 &windowPoint,
                                                            const Vector2i &viewport);
        /// Window coordinates (y down) to canvas space (y up)
        [[nodiscard]] static Math::Vector2 WindowToCanvas(const Math::Vector2 &windowPoint, const Vector2i &viewport);

        /**
         * The hit provider Application installs on its PointerDispatcher (SetUIHitProvider): HitTestScreenPoint
         * on the current scene with the main camera, the window's size and the application dispatcher's pick
         * mask; nullptr when no scene is loaded or there is no window.
         */
        [[nodiscard]] static Input::PointerDispatcher::HitProvider MakeApplicationHitProvider();

        // ===== World-space canvases =====

        /// The state a world canvas's graphics draw with (what Scene::Render's Transparent queue state becomes in
        /// Canvas::RenderInQueue): depth-tested, so opaque geometry in front hides them; no depth write, so the
        /// canvas's own graphics, all in one plane, cover each other in hierarchy order; culling off; blended
        [[nodiscard]] static constexpr Renderer::Common::RenderState WorldCanvasState()
        {
            Renderer::Common::RenderState state = Renderer::Common::RenderState::Transparent();
            state.cull = Renderer::Common::CullMode::None;
            return state;
        }

        /// Lays out one world-space root canvas in its rect (0, 0, Canvas::GetSize()) and returns its graphics in
        /// draw order (hierarchy order). Empty if it is not a world-space root canvas (Canvas::IsRootCanvas), or
        /// if it or its object is inactive.
        [[nodiscard]] static std::vector<UIDrawItem> CollectWorldCanvasGraphics(const Canvas &canvas);

        /**
         * Draws one world-space canvas, as its Canvas::RenderInQueue does: each item of
         * CollectWorldCanvasGraphics with a rect of positive size, in order, with `state` and the canvas's
         * GetCanvasToWorldMatrix, under the view and projection already set (the scene camera's). Sets no
         * renderer state.
         */
        static void RenderWorldCanvas(const Canvas &canvas, Renderer::Common::IRenderer *renderer,
                                      const Renderer::Common::RenderState &state);

        /**
         * Where `ray` crosses the plane of the canvas that `canvasToWorld` places (Canvas::GetCanvasToWorldMatrix),
         * from either side, at a distance in [0, maxDistance]. Nothing for a ray parallel to the plane (or
         * nearly), a crossing behind the ray's origin or beyond maxDistance, or a degenerate matrix (a zero scale).
         */
        [[nodiscard]] static std::optional<CanvasRayHit> RaycastCanvasPlane(
            const Math::Matrix<float, 4, 4> &canvasToWorld, const Math::Ray &ray,
            float maxDistance = std::numeric_limits<float>::infinity());

        /**
         * The world-space canvas element under a ray. For each active world-space root canvas whose plane the ray
         * crosses (RaycastCanvasPlane), the topmost raycast-target graphic whose rect contains the crossing point
         * (the last in draw order), as on an overlay canvas. Of those, the nearest along the ray wins; on a tie,
         * the later canvas in hierarchy order. Graphics outside the canvas rect can still be hit, as they still
         * draw.
         */
        [[nodiscard]] static WorldUIHit HitTestWorldCanvases(const Scene &scene, const Math::Ray &ray,
                                                             float maxDistance = std::numeric_limits<float>::infinity());

        /**
         * The whole UI hit test of a window point, in order:
         * 1. Screen-space overlay canvases (HitTestWindowPoint): their hit wins.
         * 2. Otherwise, with a camera, the nearest world-space canvas element along camera.ScreenPointToRay,
         *    between the near and far planes (HitTestWorldCanvases), unless a solid (non-trigger) physics
         *    collider on a layer in `physicsMask` is hit nearer along the same ray (Physics::Raycast::All with
         *    QueryTriggers::Ignore): then nullptr, so the dispatcher's world pick finds that collider.
         *    Trigger colliders never block, and neither does a collider the ray starts inside (a hit within
         *    StartInsideTolerance of the ray's origin, such as a player capsule around the camera). A collider
         *    less than BlockTolerance in front of the canvas (a wall it is mounted on) doesn't block it.
         * The blocking query goes to the application's physics backend (Application::Get3DPhysicsBackend), not
         * to `scene`; without a backend nothing blocks. Geometry without a collider never blocks: a canvas
         * hidden behind a mesh-only wall still takes clicks.
         * A point outside the viewport hits nothing.
         */
        [[nodiscard]] static GameObject* HitTestScreenPoint(const Scene &scene, const Camera *camera,
                                                            const Math::Vector2 &windowPoint,
                                                            const Vector2i &viewport, std::uint32_t physicsMask);

        /// A physics hit this close to the ray's origin (world units) is the ray starting inside a collider,
        /// which doesn't block a world canvas
        static constexpr float StartInsideTolerance = 1e-4f;
        /// A collider must be more than this (world units) in front of a world canvas to block it
        static constexpr float BlockTolerance = 1e-4f;

        /// Orthographic projection of canvas space onto the viewport: x 0..width, y 0..height (y up), z -1..1.
        /// Row-major, like Camera's matrices.
        [[nodiscard]] static Math::Matrix<float, 4, 4> OverlayProjection(const Vector2i &viewport);

        /// The state every UI draw gets: no depth test or depth write (UI is drawn over the scene in order),
        /// culling off, blending on
        [[nodiscard]] static constexpr Renderer::Common::RenderState OverlayState()
        {
            Renderer::Common::RenderState state;
            state.depthTest = false;
            state.depthWrite = false;
            state.cull = Renderer::Common::CullMode::None;
            state.blend = true;
            return state;
        }

        /**
         * A new root object for a canvas: on the UI layer, with a Canvas in `renderMode` (a screen-space overlay
         * by default). A WorldSpace one also gets a Positionable scaled to 0.01 (a canvas unit is a hundredth of
         * a world unit, as on Unity's usual world canvas) and a RectTransform (Unity's default 100 x 100 canvas
         * units, so 1 x 1 world units).
         */
        static std::shared_ptr<GameObject> CreateCanvas(
            const std::string &name = "Canvas", CanvasRenderMode renderMode = CanvasRenderMode::ScreenSpaceOverlay);
        /// A new UI element: on the UI layer, with a RectTransform (Unity's defaults: a 100x100 box at the
        /// parent's centre). Parent it under a canvas or another element.
        static std::shared_ptr<GameObject> CreateElement(const std::string &name = "UIElement");
        /// A new text element: CreateElement with a UIText showing `text` (the default settings: 24 px,
        /// white, top-left, wrapped to the rect, not a raycast target)
        static std::shared_ptr<GameObject> CreateText(const std::string &name = "Text", const std::string &text = "");
        /// A new button: CreateElement sized 160x40 with a white Image (the raycast target and the tinted
        /// graphic) and a Button, and a child "Label" (CreateText, stretched over the button, centred both ways,
        /// 20 px, dark grey) showing `label`. Parent it under a canvas or another element.
        static std::shared_ptr<GameObject> CreateButton(const std::string &name = "Button",
                                                        const std::string &label = "Button");
    };
}
