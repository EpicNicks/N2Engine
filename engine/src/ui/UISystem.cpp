#include "engine/ui/UISystem.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <ranges>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIGraphic.hpp"
#include "engine/ui/UIText.hpp"

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

        struct RootCanvas
        {
            GameObject *gameObject = nullptr;
            Canvas *canvas = nullptr;
            int sortOrder = 0;
        };

        /// Finds the root canvases under an object: the first Canvas on each path down (a Canvas below one is
        /// part of the outer canvas's tree). Inactive objects and their subtrees are skipped, and so is the
        /// tree of a disabled Canvas.
        void FindCanvases(const GameObject::Ptr &gameObject, std::vector<RootCanvas> &out)
        {
            if (!gameObject || !gameObject->IsActiveInHierarchy())
            {
                return;
            }
            if (gameObject->HasComponent<Canvas>())
            {
                if (Canvas *canvas = ActiveCanvasOn(*gameObject))
                {
                    out.push_back(RootCanvas{gameObject.get(), canvas, canvas->GetSortOrder()});
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
            // The canvas covers its rect (the viewport, or a world canvas's size) whatever its RectTransform's
            // anchors say; an object without one fills its parent
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

        /// Every root canvas in the scene, in hierarchy order (both render modes)
        std::vector<RootCanvas> FindAllCanvases(const Scene &scene)
        {
            std::vector<RootCanvas> canvases;
            for (const auto &root : scene.GetRootGameObjects())
            {
                FindCanvases(root, canvases);
            }
            return canvases;
        }

        /// The canvas rect of a world-space canvas: (0, 0, its size)
        Rect WorldCanvasRect(const Canvas &canvas)
        {
            const Math::Vector2 size = canvas.GetSize();
            return Rect{0.0f, 0.0f, size.x, size.y};
        }
    }

    std::vector<UIDrawItem> UISystem::CollectGraphics(const Scene &scene, const Vector2i &viewport)
    {
        std::vector<UIDrawItem> items;
        if (viewport[0] <= 0 || viewport[1] <= 0)
        {
            return items;
        }

        std::vector<RootCanvas> canvases = FindAllCanvases(scene);
        // World-space canvases draw in the scene's Transparent queue and are hit by the camera's ray instead
        std::erase_if(canvases, [](const RootCanvas &canvas) { return canvas.canvas->IsWorldSpace(); });
        // Lower sortOrder first (underneath); ties keep hierarchy order
        std::ranges::stable_sort(canvases, std::less{}, &RootCanvas::sortOrder);

        const Rect canvasRect{0.0f, 0.0f, static_cast<float>(viewport[0]), static_cast<float>(viewport[1])};
        for (const RootCanvas &canvas : canvases)
        {
            LayOut(*canvas.gameObject, canvasRect, true, items);
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
            Application &application = Application::GetInstance();
            const Vector2i viewport = application.GetWindow().GetWindowDimensions();
            return HitTestScreenPoint(SceneManager::GetCurSceneRef(), application.GetMainCamera(), windowPoint,
                                      viewport, application.GetPointerDispatcher().GetPickMask());
        };
    }

    std::vector<UIDrawItem> UISystem::CollectWorldCanvasGraphics(const Canvas &canvas)
    {
        std::vector<UIDrawItem> items;
        GameObject &gameObject = canvas.GetGameObject();
        if (!canvas.IsWorldSpace() || !canvas.IsActive() || !gameObject.IsActiveInHierarchy() ||
            !canvas.IsRootCanvas())
        {
            return items;
        }
        LayOut(gameObject, WorldCanvasRect(canvas), true, items);
        return items;
    }

    void UISystem::RenderWorldCanvas(const Canvas &canvas, Renderer::Common::IRenderer *renderer,
                                     const Renderer::Common::RenderState &state)
    {
        if (!renderer)
        {
            return;
        }
        const std::vector<UIDrawItem> items = CollectWorldCanvasGraphics(canvas);
        if (items.empty())
        {
            return;
        }
        const Math::Matrix<float, 4, 4> canvasToWorld = canvas.GetCanvasToWorldMatrix();
        for (const UIDrawItem &item : items)
        {
            if (item.rect.HasArea())
            {
                item.graphic->RenderUI(renderer, item.rect, state, canvasToWorld);
            }
        }
    }

    std::optional<CanvasRayHit> UISystem::RaycastCanvasPlane(const Math::Matrix<float, 4, 4> &canvasToWorld,
                                                             const Math::Ray &ray, const float maxDistance)
    {
        // The canvas's axes and origin in the world: its columns (column vectors)
        const Math::Vector3 axisX{canvasToWorld(0, 0), canvasToWorld(1, 0), canvasToWorld(2, 0)};
        const Math::Vector3 axisY{canvasToWorld(0, 1), canvasToWorld(1, 1), canvasToWorld(2, 1)};
        const Math::Vector3 origin{canvasToWorld(0, 3), canvasToWorld(1, 3), canvasToWorld(2, 3)};

        const Math::Vector3 normal = axisX.Cross(axisY);
        const float normalLength = normal.Length();
        const float directionLength = ray.direction.Length();
        if (!(normalLength > 0.0f) || !(directionLength > 0.0f) || !std::isfinite(normalLength) ||
            !std::isfinite(directionLength))
        {
            return std::nullopt; // a zero scale, or no direction
        }

        // The ray parallel to the plane (within about 1e-6 of the cosine) never crosses it usefully
        const float denominator = normal.Dot(ray.direction);
        if (!(std::abs(denominator) > 1e-6f * normalLength * directionLength))
        {
            return std::nullopt;
        }
        const float distance = normal.Dot(origin - ray.origin) / denominator;
        if (!std::isfinite(distance) || distance < 0.0f || distance > maxDistance)
        {
            return std::nullopt; // behind the ray's origin (the camera), or too far
        }

        // The point in canvas units: solve offset = u * axisX + v * axisY (the axes may be scaled unevenly or
        // sheared by a parent's scale, so not just a projection on each)
        const Math::Vector3 offset = ray.GetPoint(distance) - origin;
        const float xx = axisX.Dot(axisX);
        const float xy = axisX.Dot(axisY);
        const float yy = axisY.Dot(axisY);
        const float determinant = xx * yy - xy * xy;
        if (!(determinant > 0.0f) || !std::isfinite(determinant))
        {
            return std::nullopt;
        }
        const float ox = offset.Dot(axisX);
        const float oy = offset.Dot(axisY);
        CanvasRayHit hit;
        hit.distance = distance;
        hit.canvasPoint = Math::Vector2{(yy * ox - xy * oy) / determinant, (xx * oy - xy * ox) / determinant};
        if (!std::isfinite(hit.canvasPoint.x) || !std::isfinite(hit.canvasPoint.y))
        {
            return std::nullopt;
        }
        return hit;
    }

    WorldUIHit UISystem::HitTestWorldCanvases(const Scene &scene, const Math::Ray &ray, const float maxDistance)
    {
        WorldUIHit best;
        for (const RootCanvas &root : FindAllCanvases(scene))
        {
            if (!root.canvas->IsWorldSpace())
            {
                continue;
            }
            const std::optional<CanvasRayHit> planeHit =
                RaycastCanvasPlane(root.canvas->GetCanvasToWorldMatrix(), ray, maxDistance);
            // Nearer wins; a tie goes to the later canvas (as hierarchy order would draw it last)
            if (!planeHit || (best.gameObject && planeHit->distance > best.distance))
            {
                continue;
            }
            const std::vector<UIDrawItem> items = CollectWorldCanvasGraphics(*root.canvas);
            for (const UIDrawItem &item : items | std::views::reverse)
            {
                if (item.graphic->GetRaycastTarget() && item.rect.Contains(planeHit->canvasPoint))
                {
                    best = WorldUIHit{&item.graphic->GetGameObject(), root.canvas, planeHit->distance,
                                      planeHit->canvasPoint};
                    break;
                }
            }
        }
        return best;
    }

    GameObject* UISystem::HitTestScreenPoint(const Scene &scene, const Camera *camera,
                                             const Math::Vector2 &windowPoint, const Vector2i &viewport,
                                             const std::uint32_t physicsMask)
    {
        // Overlay canvases first: they are drawn over everything. This also rejects a point outside the viewport.
        if (GameObject *overlayHit = HitTestWindowPoint(scene, windowPoint, viewport))
        {
            return overlayHit;
        }
        if (!camera || viewport[0] <= 0 || viewport[1] <= 0 ||
            !(windowPoint.x >= 0.0f && windowPoint.x < static_cast<float>(viewport[0]) &&
              windowPoint.y >= 0.0f && windowPoint.y < static_cast<float>(viewport[1])))
        {
            return nullptr;
        }

        // The world pick's ray (PointerDispatcher::PickWorld): from the near plane to the far plane
        float nearToFar = 0.0f;
        const Math::Ray ray = camera->ScreenPointToRay(windowPoint, viewport, &nearToFar);
        if (!(nearToFar > 0.0f) || !std::isfinite(nearToFar))
        {
            return nullptr;
        }
        const WorldUIHit hit = HitTestWorldCanvases(scene, ray, nearToFar);
        if (!hit.gameObject)
        {
            return nullptr;
        }

        // A collider nearer along the same ray is in front of the canvas: let the world pick have it
        Physics::RaycastHit physicsHit;
        if (Physics::Raycast::Single(ray.origin, ray.direction, physicsHit, hit.distance, physicsMask) &&
            physicsHit.distance < hit.distance)
        {
            return nullptr;
        }
        return hit.gameObject;
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

    std::shared_ptr<GameObject> UISystem::CreateCanvas(const std::string &name, const CanvasRenderMode renderMode)
    {
        auto gameObject = GameObject::Create(name);
        gameObject->SetLayer(Layers::UI);
        Canvas *canvas = gameObject->AddComponent<Canvas>();
        if (canvas && renderMode == CanvasRenderMode::WorldSpace)
        {
            canvas->SetRenderMode(renderMode); // adds the Positionable and the RectTransform
            if (Positionable *positionable = gameObject->GetPositionable())
            {
                positionable->SetLocalScale(Math::Vector3{0.01f, 0.01f, 0.01f});
            }
        }
        return gameObject;
    }

    std::shared_ptr<GameObject> UISystem::CreateElement(const std::string &name)
    {
        auto gameObject = GameObject::Create(name);
        gameObject->SetLayer(Layers::UI);
        gameObject->AddComponent<RectTransform>();
        return gameObject;
    }

    std::shared_ptr<GameObject> UISystem::CreateText(const std::string &name, const std::string &text)
    {
        auto gameObject = CreateElement(name);
        if (UIText *uiText = gameObject->AddComponent<UIText>())
        {
            uiText->SetText(text);
        }
        return gameObject;
    }

    std::shared_ptr<GameObject> UISystem::CreateButton(const std::string &name, const std::string &label)
    {
        auto gameObject = CreateElement(name);
        if (auto *rectTransform = gameObject->GetComponent<RectTransform>())
        {
            rectTransform->SetSizeDelta(Math::Vector2{160.0f, 40.0f});
        }
        gameObject->AddComponent<Image>(); // white, a raycast target: what the pointer hits and the tint shows on
        gameObject->AddComponent<Button>(); // tints the first graphic on its object, the Image

        auto labelObject = CreateText("Label", label);
        if (auto *rectTransform = labelObject->GetComponent<RectTransform>())
        {
            rectTransform->StretchToParent();
        }
        if (auto *text = labelObject->GetComponent<UIText>())
        {
            text->SetFontSize(20.0f);
            text->SetHorizontalAlign(Text::HorizontalAlign::Center);
            text->SetVerticalAlign(Text::VerticalAlign::Middle);
            text->SetWrap(false);
            text->SetColor(Common::Color{0.196f, 0.196f, 0.196f, 1.0f}); // Unity's label grey (50, 50, 50)
        }
        gameObject->AddChild(labelObject, false);
        return gameObject;
    }
}
