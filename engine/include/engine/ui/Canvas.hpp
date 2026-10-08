#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/IRenderable.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::UI
{
    /// How a canvas is drawn and hit (Unity's Canvas.renderMode, without Screen Space - Camera)
    enum class CanvasRenderMode : std::uint8_t
    {
        /// Over the whole viewport, after the scene, in the UI pass (the default)
        ScreenSpaceOverlay,
        /// A rectangle placed in the world by its object's Positionable, drawn in the scene's Transparent queue
        WorldSpace
    };

    // Saved by name. A missing key (a scene saved before world canvases) keeps the default, overlay; an unknown
    // name reads as the first entry, overlay too.
    N2_SERIALIZE_ENUM(CanvasRenderMode, {
                      { CanvasRenderMode::ScreenSpaceOverlay, "ScreenSpaceOverlay" },
                      { CanvasRenderMode::WorldSpace, "WorldSpace" }
                      })

    /**
     * The root of a UI tree. Its object's descendants with UI components (Image, ...) are laid out inside the
     * canvas rect and drawn by UISystem.
     *
     * Only canvases with no Canvas above them in the hierarchy count. A Canvas under another canvas is ignored
     * (its object is laid out and drawn as an ordinary element of the outer canvas), whether it is enabled
     * or not, and whatever its render mode. A disabled root Canvas component, like an inactive object, hides
     * its whole tree from drawing and hit tests. Of several Canvas components on one object, the first enabled
     * one is the canvas.
     *
     * Screen-space overlay (the default): the canvas covers the whole viewport. Its rect is (0, 0, width,
     * height) in window coordinates' units, whatever the RectTransform on its object says. Overlay canvases
     * draw in the UI pass after the scene, in ascending sortOrder (ties in hierarchy order), so a higher
     * sortOrder is on top and is hit first.
     *
     * World space: the canvas is a rectangle in the world.
     * - Size: its rect is (0, 0, width, height) in canvas units, the size being its own RectTransform's
     *   sizeDelta (GetSize; anchors and anchoredPosition are ignored, as the canvas has no parent rect), or
     *   100 x 100 without a RectTransform. Children lay out inside it exactly as on an overlay canvas.
     * - Placement: its object's Positionable places the canvas's pivot (the RectTransform's pivot, the centre
     *   without one) in the world, and its rotation and scale apply too. The scale is the size of one canvas
     *   unit in world units: Unity commonly uses 0.01, so a 400 x 300 canvas is 4 x 3 world units. The canvas
     *   lies in its object's local x/y plane, facing +Z (GetCanvasToWorldMatrix). An object without a
     *   Positionable is at the origin, unrotated, with scale 1.
     * - Drawing: the canvas is an IRenderable in the Transparent queue, so the whole canvas is one transparent
     *   item, sorted with the scene's other transparent renderables by its object's position. Inside it, its
     *   graphics draw in hierarchy order with the scene camera's view and projection and UISystem's
     *   WorldCanvasState (depth-tested, no depth write, no culling, blended). sortOrder is not used.
     * - Hit testing: by the camera's ray, after the overlay canvases; see UISystem::HitTestScreenPoint.
     * An overlay canvas is a renderable too, but it draws in neither queue (DrawsInQueue is false).
     */
    class Canvas final : public IRenderable
    {
    public:
        using Matrix4 = Math::Matrix<float, 4, 4>;

        explicit Canvas(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Canvas"; }

        /// The overlay draw and hit order (not used by world-space canvases)
        [[nodiscard]] int GetSortOrder() const { return _sortOrder; }
        void SetSortOrder(const int sortOrder) { _sortOrder = sortOrder; }

        [[nodiscard]] CanvasRenderMode GetRenderMode() const { return _renderMode; }
        /// Switching to WorldSpace gives the object a Positionable (at the origin, scale 1) and a RectTransform
        /// (Unity's 100 x 100 default) if it has none. Nothing is removed on switching back.
        void SetRenderMode(CanvasRenderMode renderMode);
        [[nodiscard]] bool IsWorldSpace() const { return _renderMode == CanvasRenderMode::WorldSpace; }

        /// A world-space canvas's size in canvas units: its RectTransform's sizeDelta, or 100 x 100 without one
        [[nodiscard]] Math::Vector2 GetSize() const;
        /// Sets the RectTransform's sizeDelta, adding a RectTransform first if the object has none
        void SetSize(const Math::Vector2 &size);
        /// The point of the canvas its object's position places: the RectTransform's pivot, or (0.5, 0.5)
        [[nodiscard]] Math::Vector2 GetPivot() const;

        /**
         * Canvas space (units, y up, origin at the canvas rect's bottom-left) to world space, for a world-space
         * canvas: the object's local-to-world matrix times a translation by -pivot * size. Column vectors,
         * row-major, like every engine matrix. It is what a world canvas's graphics' model matrices start with.
         */
        [[nodiscard]] Matrix4 GetCanvasToWorldMatrix() const;

        /// Whether this is the canvas UISystem uses for its object: the first enabled Canvas there, with no
        /// Canvas (enabled or not) on any object above it. Ignores whether the object is active.
        [[nodiscard]] bool IsRootCanvas() const;

        // IRenderable: a world-space root canvas draws in the Transparent queue; any other draws nowhere
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override;
        [[nodiscard]] bool DrawsInQueue(RenderQueue queue) const override;
        /// RenderInQueue in the Transparent queue with RenderState::Transparent(), as Scene::Render would
        void Render(Renderer::Common::IRenderer *renderer) override;
        /// A world-space root canvas draws its graphics (UISystem::RenderWorldCanvas) with `state`, culling
        /// turned off. Anything else draws nothing.
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        /// The graphics hold their own resources: nothing to do
        void InitializeRenderResources(Renderer::Common::IRenderer *) override {}
        void CleanupRenderResources(Renderer::Common::IRenderer *) override {}

        /// One per object: AddComponent returns the existing one
        static constexpr bool IsSingleton = true;

        /// Unity's default size for a new RectTransform, used when a world canvas has none
        static constexpr float DefaultWorldSize = 100.0f;

    private:
        int _sortOrder = 0;
        CanvasRenderMode _renderMode = CanvasRenderMode::ScreenSpaceOverlay;
    };
}
