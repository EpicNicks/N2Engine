#include "engine/ui/Canvas.hpp"

#include <math/Vector3.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::UI
{
    Canvas::Canvas(GameObject &gameObject) : IRenderable(gameObject)
    {
        RegisterMember("sortOrder", _sortOrder);
        RegisterMember("renderMode", _renderMode);
    }

    void Canvas::SetRenderMode(const CanvasRenderMode renderMode)
    {
        _renderMode = renderMode;
        if (renderMode == CanvasRenderMode::WorldSpace)
        {
            GameObject &gameObject = GetGameObject();
            gameObject.CreatePositionable(); // does nothing if it has one
            if (!gameObject.HasComponent<RectTransform>())
            {
                gameObject.AddComponent<RectTransform>();
            }
        }
    }

    Math::Vector2 Canvas::GetSize() const
    {
        if (const RectTransform *rectTransform = GetGameObject().GetComponent<RectTransform>())
        {
            return rectTransform->GetSizeDelta();
        }
        return Math::Vector2{DefaultWorldSize, DefaultWorldSize};
    }

    void Canvas::SetSize(const Math::Vector2 &size)
    {
        GameObject &gameObject = GetGameObject();
        RectTransform *rectTransform = gameObject.GetComponent<RectTransform>();
        if (!rectTransform)
        {
            rectTransform = gameObject.AddComponent<RectTransform>();
        }
        if (rectTransform)
        {
            rectTransform->SetSizeDelta(size);
        }
    }

    Math::Vector2 Canvas::GetPivot() const
    {
        if (const RectTransform *rectTransform = GetGameObject().GetComponent<RectTransform>())
        {
            return rectTransform->GetPivot();
        }
        return Math::Vector2{0.5f, 0.5f};
    }

    Canvas::Matrix4 Canvas::GetCanvasToWorldMatrix() const
    {
        const Math::Vector2 size = GetSize();
        const Math::Vector2 pivot = GetPivot();
        // The pivot goes to the object's origin
        const Matrix4 pivotToOrigin =
            Matrix4::Translation(Math::Vector3{-pivot.x * size.x, -pivot.y * size.y, 0.0f});
        if (const Positionable *positionable = GetGameObject().GetPositionable())
        {
            return positionable->GetLocalToWorldMatrix() * pivotToOrigin;
        }
        return pivotToOrigin;
    }

    bool Canvas::IsRootCanvas() const
    {
        const GameObject &gameObject = GetGameObject();
        // The first enabled Canvas on the object is the canvas (UISystem's rule)
        bool first = false;
        for (const Canvas *canvas : gameObject.GetComponents<Canvas>())
        {
            if (canvas && canvas->IsActive())
            {
                first = canvas == this;
                break;
            }
        }
        if (!first)
        {
            return false;
        }
        for (GameObject::Ptr parent = gameObject.GetParent(); parent; parent = parent->GetParent())
        {
            if (parent->HasComponent<Canvas>())
            {
                return false; // part of the outer canvas's tree
            }
        }
        return true;
    }

    RenderQueueKey Canvas::GetRenderQueue() const
    {
        if (IsWorldSpace())
        {
            return RenderQueueKey{RenderQueue::Transparent, 0};
        }
        return RenderQueueKey{};
    }

    bool Canvas::DrawsInQueue(const RenderQueue queue) const
    {
        return queue == RenderQueue::Transparent && IsWorldSpace() && IsRootCanvas();
    }

    void Canvas::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void Canvas::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                               const RenderQueue queue)
    {
        if (!renderer || !DrawsInQueue(queue))
        {
            return;
        }
        // Both faces: a canvas seen from behind still shows (mirrored), as in Unity
        Renderer::Common::RenderState canvasState = state;
        canvasState.cull = Renderer::Common::CullMode::None;
        UISystem::RenderWorldCanvas(*this, renderer, canvasState);
    }
}
