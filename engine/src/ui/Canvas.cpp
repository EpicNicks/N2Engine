#include "engine/ui/Canvas.hpp"

#include <algorithm>
#include <span>
#include <string>

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

    void Canvas::OnEditorFieldsChanged(const std::span<const std::string> changed)
    {
        if (std::ranges::find(changed, "renderMode") != changed.end())
        {
            SetRenderMode(_renderMode);
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

    std::optional<BoundingBox> Canvas::GetWorldBounds() const
    {
        if (!IsWorldSpace() || !IsRootCanvas())
        {
            return std::nullopt;
        }
        return ComputeWorldBounds(UISystem::CollectWorldCanvasGraphics(*this));
    }

    std::optional<BoundingBox> Canvas::ComputeWorldBounds(const std::span<const UIDrawItem> graphics) const
    {
        if (!IsWorldSpace() || !IsRootCanvas())
        {
            return std::nullopt;
        }
        // The canvas rect, and the rects of the graphics that draw (in canvas space, y up)
        const Math::Vector2 size = GetSize();
        bool any = false;
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        const auto cover = [&](const Rect &rect)
        {
            if (!rect.HasArea())
            {
                return;
            }
            minX = any ? std::min(minX, rect.XMin()) : rect.XMin();
            minY = any ? std::min(minY, rect.YMin()) : rect.YMin();
            maxX = any ? std::max(maxX, rect.XMax()) : rect.XMax();
            maxY = any ? std::max(maxY, rect.YMax()) : rect.YMax();
            any = true;
        };
        cover(Rect{0.0f, 0.0f, size.x, size.y});
        for (const UIDrawItem &item : graphics)
        {
            cover(item.rect);
        }
        if (!any)
        {
            return std::nullopt;
        }
        const BoundingBox local{Math::Vector3{minX, minY, 0.0f}, Math::Vector3{maxX, maxY, 0.0f}};
        return local.Transformed(GetCanvasToWorldMatrix());
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
