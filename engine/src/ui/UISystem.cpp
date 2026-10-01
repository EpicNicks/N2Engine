#include "engine/ui/UISystem.hpp"

#include <algorithm>
#include <ranges>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    namespace
    {
        /// The first active Canvas on an object, or nullptr
        Canvas* ActiveCanvasOn(const GameObject &gameObject)
        {
            for (Canvas *canvas : gameObject.GetComponents<Canvas>())
            {
                if (canvas && canvas->IsActive())
                {
                    return canvas;
                }
            }
            return nullptr;
        }

        /// Finds the root canvases under an object: the first Canvas on each path down (a Canvas below one is
        /// part of the outer canvas's tree). Inactive objects and their subtrees are skipped, and so is the
        /// tree of a disabled Canvas.
        void FindCanvases(const GameObject::Ptr &gameObject, std::vector<GameObject *> &out)
        {
            if (!gameObject || !gameObject->IsActiveInHierarchy())
            {
                return;
            }
            if (gameObject->HasComponent<Canvas>())
            {
                if (ActiveCanvasOn(*gameObject))
                {
                    out.push_back(gameObject.get());
                }
                return;
            }
            for (const auto &child : gameObject->GetChildren())
            {
                FindCanvases(child, out);
            }
        }

        void LayOut(GameObject &gameObject, const Rect &parentRect, const bool isCanvas, std::vector<UIDrawItem> &out)
        {
            if (!gameObject.IsActiveInHierarchy())
            {
                return;
            }

            RectTransform *rectTransform = gameObject.GetComponent<RectTransform>();
            // The canvas covers the viewport whatever its RectTransform says; an object without one fills its
            // parent
            Rect rect = parentRect;
            if (rectTransform && !isCanvas)
            {
                rect = rectTransform->ResolveIn(parentRect);
            }
            if (rectTransform)
            {
                rectTransform->SetResolvedRect(rect);
            }

            for (UIGraphic *graphic : gameObject.GetComponents<UIGraphic>())
            {
                if (graphic && graphic->IsActive())
                {
                    out.push_back(UIDrawItem{graphic, rect});
                }
            }

            for (const auto &child : gameObject.GetChildren())
            {
                if (child)
                {
                    LayOut(*child, rect, false, out);
                }
            }
        }
    }

    std::vector<UIDrawItem> UISystem::CollectGraphics(const Scene &scene, const Vector2i &viewport)
    {
        std::vector<UIDrawItem> items;
        if (viewport[0] <= 0 || viewport[1] <= 0)
        {
            return items;
        }

        std::vector<GameObject *> canvasObjects;
        for (const auto &root : scene.GetRootGameObjects())
        {
            FindCanvases(root, canvasObjects);
        }
        // Lower sortOrder first (underneath); ties keep hierarchy order
        std::ranges::stable_sort(canvasObjects, [](const GameObject *a, const GameObject *b)
        {
            return ActiveCanvasOn(*a)->GetSortOrder() < ActiveCanvasOn(*b)->GetSortOrder();
        });

        const Rect canvasRect{0.0f, 0.0f, static_cast<float>(viewport[0]), static_cast<float>(viewport[1])};
        for (GameObject *canvasObject : canvasObjects)
        {
            LayOut(*canvasObject, canvasRect, true, items);
        }
        return items;
    }

    void UISystem::Render(const Scene &scene, Renderer::Common::IRenderer *renderer, const Vector2i &viewport)
    {
        if (!renderer)
        {
            return;
        }
        const std::vector<UIDrawItem> items = CollectGraphics(scene, viewport);
        if (items.empty())
        {
            return;
        }

        const Math::Matrix<float, 4, 4> view = Math::Matrix<float, 4, 4>::identity();
        const Math::Matrix<float, 4, 4> projection = OverlayProjection(viewport);
        renderer->SetViewProjection(view.Data(), projection.Data());

        constexpr Renderer::Common::RenderState state = OverlayState();
        for (const UIDrawItem &item : items)
        {
            if (item.rect.HasArea())
            {
                item.graphic->RenderUI(renderer, item.rect, state);
            }
        }
    }

    GameObject* UISystem::HitTest(const Scene &scene, const Math::Vector2 &canvasPoint, const Vector2i &viewport)
    {
        const std::vector<UIDrawItem> items = CollectGraphics(scene, viewport);
        // Topmost first: the reverse of draw order
        for (const UIDrawItem &item : items | std::views::reverse)
        {
            if (item.graphic->GetRaycastTarget() && item.rect.Contains(canvasPoint))
            {
                return &item.graphic->GetGameObject();
            }
        }
        return nullptr;
    }

    Math::Vector2 UISystem::WindowToCanvas(const Math::Vector2 &windowPoint, const Vector2i &viewport)
    {
        return Math::Vector2{windowPoint.x, static_cast<float>(viewport[1]) - windowPoint.y};
    }

    GameObject* UISystem::HitTestWindowPoint(const Scene &scene, const Math::Vector2 &windowPoint,
                                             const Vector2i &viewport)
    {
        // Only inside the viewport, as the world pick (PointerDispatcher::PickWorld)
        if (!(windowPoint.x >= 0.0f && windowPoint.x < static_cast<float>(viewport[0]) &&
              windowPoint.y >= 0.0f && windowPoint.y < static_cast<float>(viewport[1])))
        {
            return nullptr;
        }
        return HitTest(scene, WindowToCanvas(windowPoint, viewport), viewport);
    }

    Input::PointerDispatcher::HitProvider UISystem::MakeApplicationHitProvider()
    {
        return [](const Math::Vector2 &windowPoint) -> GameObject *
        {
            if (SceneManager::GetCurSceneIndex() == -1)
            {
                return nullptr;
            }
            const Vector2i viewport = Application::GetInstance().GetWindow().GetWindowDimensions();
            return HitTestWindowPoint(SceneManager::GetCurSceneRef(), windowPoint, viewport);
        };
    }

    Math::Matrix<float, 4, 4> UISystem::OverlayProjection(const Vector2i &viewport)
    {
        const float width = viewport[0] > 0 ? static_cast<float>(viewport[0]) : 1.0f;
        const float height = viewport[1] > 0 ? static_cast<float>(viewport[1]) : 1.0f;

        // Camera::SetOrthographic(0, width, 0, height, -1, 1), written out
        Math::Matrix<float, 4, 4> projection; // zero-filled
        projection(0, 0) = 2.0f / width;
        projection(0, 3) = -1.0f;
        projection(1, 1) = 2.0f / height;
        projection(1, 3) = -1.0f;
        projection(2, 2) = -1.0f;
        projection(3, 3) = 1.0f;
        return projection;
    }

    std::shared_ptr<GameObject> UISystem::CreateCanvas(const std::string &name)
    {
        auto gameObject = GameObject::Create(name);
        gameObject->SetLayer(Layers::UI);
        gameObject->AddComponent<Canvas>();
        return gameObject;
    }

    std::shared_ptr<GameObject> UISystem::CreateElement(const std::string &name)
    {
        auto gameObject = GameObject::Create(name);
        gameObject->SetLayer(Layers::UI);
        gameObject->AddComponent<RectTransform>();
        return gameObject;
    }
}
