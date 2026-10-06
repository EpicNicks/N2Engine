#pragma once

#include <memory>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/VectorN.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/input/PointerDispatcher.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine
{
    class GameObject;
    class Scene;
}

namespace N2Engine::UI
{
    class Canvas;
    class UIGraphic;

    /// One graphic of the UI pass and the rect it draws over (canvas space)
    struct UIDrawItem
    {
        UIGraphic *graphic = nullptr;
        Rect rect{};
    };

    /**
     * Screen-space UI: layout, the UI pass, and the UI hit test. Stateless; everything is worked out from the
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
     */
    class UISystem
    {
    public:
        /// Lays out every canvas in the scene for the viewport and returns its graphics in draw order. Empty for
        /// an empty viewport.
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

        /// The hit provider Application installs on its PointerDispatcher (SetUIHitProvider): HitTestWindowPoint
        /// on the current scene with the window's size; nullptr when no scene is loaded or there is no window
        [[nodiscard]] static Input::PointerDispatcher::HitProvider MakeApplicationHitProvider();

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

        /// A new root object for a canvas: on the UI layer, with a Canvas
        static std::shared_ptr<GameObject> CreateCanvas(const std::string &name = "Canvas");
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
