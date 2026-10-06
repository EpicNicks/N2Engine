#pragma once

#include <math/Matrix.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/common/Color.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    /**
     * A UI component that draws in the UI pass (Image, UIText). It is not an IRenderable, so the scene
     * pass (Scene::Render) never draws it itself. UISystem draws every active graphic on an active object
     * under a canvas, in hierarchy order, with the rect the layout resolved for its object: in the UI pass for
     * a screen-space overlay canvas, and inside its canvas's single Transparent-queue draw for a world-space
     * canvas (see Canvas).
     *
     * A graphic with raycastTarget set (the default, except for UIText) is what the UI hit test finds: the
     * pointer over it goes to its object (OnMouse* callbacks) and not to the world behind it.
     */
    class UIGraphic : public SerializableComponent
    {
    public:
        using Matrix4 = Math::Matrix<float, 4, 4>;

        /**
         * Draws this graphic over `rect` (canvas space, already known to have an area), with the given render
         * state. `canvasToWorld` maps canvas space to the space the renderer's view expects: identity on a
         * screen-space overlay canvas, whose view is identity and projection the canvas's (UISystem::Render),
         * and the canvas's world matrix (Canvas::GetCanvasToWorldMatrix) on a world-space canvas, drawn with the
         * scene camera's view and projection. The graphic's model matrix is canvasToWorld times the matrix that
         * places it in its rect (ComposeModel). Like IRenderable::Render, this must not add, remove or destroy
         * components or GameObjects immediately.
         */
        virtual void RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                              const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld) = 0;

        /// RenderUI on a screen-space overlay canvas: canvasToWorld is identity
        void RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                      const Renderer::Common::RenderState &state)
        {
            RenderUI(renderer, rect, state, Matrix4::identity());
        }

        /// canvasToWorld * local, or exactly `local` when canvasToWorld is identity (an overlay canvas)
        [[nodiscard]] static Matrix4 ComposeModel(const Matrix4 &canvasToWorld, const Matrix4 &local);

        [[nodiscard]] const Common::Color& GetColor() const { return _color; }
        void SetColor(const Common::Color &color) { _color = color; }

        /// A colour the graphic's own colour is multiplied by when it is drawn (white, so no change, by
        /// default). It is what a Button tints its target graphic with, kept apart from the colour so the
        /// tint never overwrites it: setting the tint back to white draws the graphic's own colour again.
        /// Not serialized (Unity's CanvasRenderer colour, which its Graphic.CrossFadeColor sets).
        [[nodiscard]] const Common::Color& GetTint() const { return _tint; }
        void SetTint(const Common::Color &tint) { _tint = tint; }
        /// The colour drawn: GetColor() multiplied by GetTint(), channel by channel (alpha too)
        [[nodiscard]] Common::Color GetDrawColor() const;

        /// Whether the UI hit test can find this graphic (true by default; false for UIText). False lets the
        /// pointer through.
        [[nodiscard]] bool GetRaycastTarget() const { return _raycastTarget; }
        void SetRaycastTarget(const bool raycastTarget) { _raycastTarget = raycastTarget; }

    protected:
        explicit UIGraphic(GameObject &gameObject);

        Common::Color _color{Common::Color::White};
        bool _raycastTarget = true;
        Common::Color _tint{Common::Color::White};
    };
}
