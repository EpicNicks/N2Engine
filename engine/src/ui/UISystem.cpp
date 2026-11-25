#include "engine/ui/UISystem.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <ranges>
#include <utility>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics2d/Physics2DQueries.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/LayoutGroup.hpp"
#include "engine/ui/RectMask.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/Slider.hpp"
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

#ifdef N2ENGINE_UI_ENABLED
        /// Whether the object has an enabled RectMask (it clips its descendants)
        bool HasActiveMask(const GameObject &gameObject)
        {
            const RectMask *mask = gameObject.GetComponent<RectMask>();
            return mask && mask->IsActive();
        }

        /// The region the masks above an object leave (canvas space): nothing without a mask; a rect while every mask
        /// is along the axes; otherwise the polygon, with `rect` the box around it
        struct MaskClip
        {
            std::optional<Rect> rect;
            ConvexPolygon polygon;
        };

        /// `clip` narrowed by a mask over `rect`, placed on the canvas by `transform`
        MaskClip Narrow(const MaskClip &clip, const Rect &rect, const Transform2D &transform)
        {
            if (transform.IsIdentity() && clip.polygon.empty())
            {
                return MaskClip{clip.rect ? Rect::Intersect(*clip.rect, rect) : rect, {}};
            }
            ConvexPolygon region = TransformedCorners(rect, transform);
            if (!clip.polygon.empty())
            {
                region = IntersectConvex(region, clip.polygon);
            }
            else if (clip.rect)
            {
                region = IntersectConvex(region, TransformedCorners(*clip.rect, Transform2D{}));
            }
            if (!(PolygonArea(region) > 0.0f))
            {
                return MaskClip{Rect{rect.x, rect.y, 0.0f, 0.0f}, {}}; // nothing shows below it
            }
            if (const std::optional<Rect> aligned = AsAxisAlignedRect(region))
            {
                return MaskClip{*aligned, {}};
            }
            const Rect bounds = BoundsOf(region);
            return MaskClip{bounds, std::move(region)};
        }

        void LayOut(GameObject &gameObject, const Rect &parentRect, const bool isCanvas, const Transform2D &parentTransform,
                    const MaskClip &clip, std::vector<UIDrawItem> &out)
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
                // Issue #445: a ContentSizeFitter sizes the object to its content first
                if (const ContentSizeFitter *fitter = gameObject.GetComponent<ContentSizeFitter>(); fitter && fitter->IsActive())
                {
                    fitter->Fit(parentRect);
                }
                rect = rectTransform->ResolveIn(parentRect);
            }
            // Rotation and scale turn the resolved rect, and everything below it, about its pivot. The canvas's own are
            // not used: it covers its rect (a world canvas is placed by its transform).
            Transform2D transform = parentTransform;
            if (rectTransform && !isCanvas)
            {
                if (const Transform2D local = rectTransform->LocalTransform(rect); !local.IsIdentity())
                {
                    transform = parentTransform * local;
                }
            }
            if (rectTransform)
            {
                rectTransform->SetResolvedRect(rect);
                rectTransform->SetLocalToCanvas(transform);
            }
            // Issue #445: a layout group places the children before they resolve their rects in it
            for (const LayoutGroup *group : gameObject.GetComponents<LayoutGroup>())
            {
                if (group && group->IsActive())
                {
                    group->Arrange(rect);
                    break;
                }
            }

            for (UIGraphic *graphic : gameObject.GetComponents<UIGraphic>())
            {
                if (graphic && graphic->IsActive())
                {
                    out.push_back(UIDrawItem{graphic, rect, clip.rect, transform, clip.polygon});
                }
            }

            // A RectMask clips the descendants (not its own object's graphics) to its rect, within any clip above it
            const bool masks = HasActiveMask(gameObject);
            const MaskClip narrowed = masks ? Narrow(clip, rect, transform) : MaskClip{};
            const MaskClip &childClip = masks ? narrowed : clip;
            for (const auto &child : gameObject.GetChildren())
            {
                if (child)
                {
                    LayOut(*child, rect, false, transform, childClip, out);
                }
            }
        }

#endif

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

        /// Sets the item's clip on its graphic in the space of the item's rect: a rect when it is one there (no turn
        /// between the graphic and its masks), else the polygon
        void SetClipOf(const UIDrawItem &item)
        {
            if (!item.clip)
            {
                return;
            }
            if (item.transform.IsIdentity() && item.clipPolygon.empty())
            {
                item.graphic->SetRenderClip(item.clip);
                return;
            }
            const std::optional<Transform2D> toLocal = item.transform.Inverse();
            if (!toLocal)
            {
                return; // a flattened item is not visible and never gets here
            }
            ConvexPolygon region = item.clipPolygon.empty() ? TransformedCorners(*item.clip, Transform2D{}) : item.clipPolygon;
            for (Math::Vector2 &point : region)
            {
                point = toLocal->Apply(point);
            }
            if (toLocal->a * toLocal->d - toLocal->b * toLocal->c < 0.0f)
            {
                std::ranges::reverse(region); // mirrored: counter-clockwise again
            }
            if (const std::optional<Rect> aligned = AsAxisAlignedRect(region))
            {
                item.graphic->SetRenderClip(*aligned);
            }
            else
            {
                item.graphic->SetRenderClipPolygon(std::move(region));
            }
        }

        /// Draws one item that can show (UIDrawItem::IsVisible), with its clip set on the graphic for the draw only
        void DrawItem(const UIDrawItem &item, Renderer::Common::IRenderer *renderer,
                      const Renderer::Common::RenderState &state, const Math::Matrix<float, 4, 4> &canvasToWorld)
        {
            if (!item.IsVisible())
            {
                return;
            }
            // Each graphic decides whether the clip cuts it (text can overflow its rect): one inside it draws as without
            SetClipOf(item);
            // Rotation and scale are part of the matrix the graphic draws with, so graphics need not know about them
            if (item.transform.IsIdentity())
            {
                item.graphic->RenderUI(renderer, item.rect, state, canvasToWorld);
            }
            else
            {
                item.graphic->RenderUI(renderer, item.rect, state,
                                       UIGraphic::ComposeModel(canvasToWorld, item.transform.ToMatrix()));
            }
            item.graphic->SetRenderClip(std::nullopt);
        }

#ifdef N2ENGINE_UI_ENABLED
        /// The canvas rect of a world-space canvas: (0, 0, its size)
        Rect WorldCanvasRect(const Canvas &canvas)
        {
            const Math::Vector2 size = canvas.GetSize();
            return Rect{0.0f, 0.0f, size.x, size.y};
        }
#endif

#ifndef N2ENGINE_UI_ENABLED
        /// Set by the one warning of a build without the UI module, so the scene is not searched for canvases again
        bool g_leftOutWarned = false;
#endif
    }

    // The UI module (N2ENGINE_USE_UI, defined on this file, Image.cpp and UIText.cpp only by engine/CMakeLists.txt). Without
    // it nothing is laid out: the two Collect functions below are empty, so nothing is drawn or hit, and the hit tests stop
    // before the ray maths. The objects, their components and the factories (CreateCanvas, ...) are the same in both builds.
    bool UIDrawItem::Covers(const Math::Vector2 &point) const
    {
        if (transform.IsIdentity())
        {
            return rect.Contains(point);
        }
        const std::optional<Transform2D> toLocal = transform.Inverse();
        return toLocal && rect.Contains(toLocal->Apply(point));
    }

    bool UIDrawItem::Hits(const Math::Vector2 &point) const
    {
        if (!Covers(point))
        {
            return false;
        }
        if (!clipPolygon.empty())
        {
            return ConvexContains(clipPolygon, point);
        }
        return !clip || clip->Contains(point);
    }

    bool UIDrawItem::IsVisible() const
    {
        if (!rect.HasArea())
        {
            return false;
        }
        if (transform.IsIdentity() && clipPolygon.empty())
        {
            return !clip || Rect::Intersect(rect, *clip).HasArea();
        }
        if (!transform.Inverse())
        {
            return false; // a zero scale
        }
        if (!clip)
        {
            return true;
        }
        const ConvexPolygon region = clipPolygon.empty() ? TransformedCorners(*clip, Transform2D{}) : clipPolygon;
        return !IntersectConvex(TransformedCorners(rect, transform), region).empty();
    }

    bool UISystem::IsInBuild()
    {
#ifdef N2ENGINE_UI_ENABLED
        return true;
#else
        return false;
#endif
    }

    void UISystem::WarnIfNotInBuild(const std::string_view who)
    {
#ifdef N2ENGINE_UI_ENABLED
        static_cast<void>(who);
#else
        if (!std::exchange(g_leftOutWarned, true))
        {
            Logger::Warn(std::format("{}: this build has no UI module (N2ENGINE_USE_UI is OFF), so UI keeps its data, draws "
                                     "nothing and is never hit",
                                     who));
        }
#endif
    }

    std::vector<UIDrawItem> UISystem::CollectGraphics(const Scene &scene, const Vector2i &viewport)
    {
#ifndef N2ENGINE_UI_ENABLED
        static_cast<void>(viewport);
        // The one warning, from the first scene that has a canvas
        if (!g_leftOutWarned)
        {
            if (const std::vector<RootCanvas> canvases = FindAllCanvases(scene); !canvases.empty())
            {
                WarnIfNotInBuild(std::format("Canvas on '{}'", canvases.front().gameObject->GetName()));
            }
        }
        return {};
#else
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
            LayOut(*canvas.gameObject, canvasRect, true, Transform2D{}, MaskClip{}, items);
        }
        return items;
#endif
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
            DrawItem(item, renderer, state, Math::Matrix<float, 4, 4>::identity());
        }
    }

    GameObject* UISystem::HitTest(const Scene &scene, const Math::Vector2 &canvasPoint, const Vector2i &viewport)
    {
        const std::vector<UIDrawItem> items = CollectGraphics(scene, viewport);
        // Topmost first: the reverse of draw order
        for (const UIDrawItem &item : items | std::views::reverse)
        {
            if (item.graphic->GetRaycastTarget() && item.Hits(canvasPoint))
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
            // The size the UI pass laid out in: the window's, or the host's render size (the editor's viewport)
            const Vector2i viewport = application.GetWindow().GetRenderDimensions();
            return HitTestScreenPoint(SceneManager::GetCurSceneRef(), application.GetMainCamera(), windowPoint,
                                      viewport, application.GetPointerDispatcher().GetPickMask());
        };
    }

    std::vector<UIDrawItem> UISystem::CollectWorldCanvasGraphics(const Canvas &canvas)
    {
#ifndef N2ENGINE_UI_ENABLED
        WarnIfNotInBuild(std::format("Canvas on '{}'", canvas.GetGameObject().GetName()));
        return {};
#else
        std::vector<UIDrawItem> items;
        GameObject &gameObject = canvas.GetGameObject();
        if (!canvas.IsWorldSpace() || !canvas.IsActive() || !gameObject.IsActiveInHierarchy() ||
            !canvas.IsRootCanvas())
        {
            return items;
        }
        LayOut(gameObject, WorldCanvasRect(canvas), true, Transform2D{}, MaskClip{}, items);
        return items;
#endif
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
            DrawItem(item, renderer, state, canvasToWorld);
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

        // The point in canvas units: solve offset = u * axisX + v * axisY (the axes need not be orthogonal or
        // equally long, e.g. with an uneven scale under a rotated parent, so not just a projection on each)
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
#ifndef N2ENGINE_UI_ENABLED
        static_cast<void>(scene);
        static_cast<void>(ray);
        static_cast<void>(maxDistance);
        return {};
#else
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
                if (item.graphic->GetRaycastTarget() && item.Hits(planeHit->canvasPoint))
                {
                    best = WorldUIHit{&item.graphic->GetGameObject(), root.canvas, planeHit->distance,
                                      planeHit->canvasPoint};
                    break;
                }
            }
        }
        return best;
#endif
    }

    GameObject* UISystem::HitTestScreenPoint(const Scene &scene, const Camera *camera,
                                             const Math::Vector2 &windowPoint, const Vector2i &viewport,
                                             const std::uint32_t physicsMask)
    {
#ifndef N2ENGINE_UI_ENABLED
        // Nothing is laid out, so nothing can be hit: the pointer goes to the world
        static_cast<void>(scene);
        static_cast<void>(camera);
        static_cast<void>(windowPoint);
        static_cast<void>(viewport);
        static_cast<void>(physicsMask);
        return nullptr;
#else
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

        // A solid collider nearer along the same ray is in front of the canvas: let the world pick have it.
        // Triggers never block (an interaction volume around a world-space button), nor does a collider the
        // ray starts inside (a first-person player's capsule around the camera), which PhysX reports at
        // distance 0. A collider within BlockTolerance in front of the canvas (the wall it is mounted on)
        // doesn't block it either, so a canvas on a collider's face isn't hit or missed by rounding.
        if (hit.distance <= 0.0f)
        {
            return hit.gameObject; // on the near plane: nothing can be in front of it
        }
        std::vector<Physics::RaycastHit> physicsHits;
        Physics::Raycast::All(ray.origin, ray.direction, physicsHits, hit.distance, physicsMask,
                              Physics::QueryTriggers::Ignore);
        for (const Physics::RaycastHit &physicsHit : physicsHits)
        {
            if (physicsHit.distance > StartInsideTolerance && physicsHit.distance < hit.distance - BlockTolerance)
            {
                return nullptr;
            }
        }
        // A solid 2D collider in front of the canvas, by the same rule (each in the plane of its object's z)
        Physics2D::RaycastHit2D hit2D;
        if (Physics2D::PickRay(ray.origin, ray.direction, hit2D, hit.distance - BlockTolerance, physicsMask,
                               Physics::QueryTriggers::Ignore) &&
            hit2D.distance > StartInsideTolerance)
        {
            return nullptr;
        }
        return hit.gameObject;
#endif
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

    bool UISystem::IsSelfOrBelow(const GameObject *object, const GameObject &ancestor)
    {
        while (object)
        {
            if (object == &ancestor)
            {
                return true;
            }
            const GameObject::Ptr parent = object->GetParent();
            object = parent.get(); // the scene owns it, so it outlives this walk
        }
        return false;
    }

    const Canvas *UISystem::RootCanvasOf(const GameObject &gameObject)
    {
        const Canvas *found = nullptr;
        for (const GameObject *object = &gameObject; object;)
        {
            for (const Canvas *canvas : object->GetComponents<Canvas>())
            {
                if (canvas && canvas->IsRootCanvas())
                {
                    found = canvas;
                }
            }
            const GameObject::Ptr parent = object->GetParent();
            object = parent.get();
        }
        return found;
    }

    std::shared_ptr<GameObject> UISystem::CreateSlider(const std::string &name)
    {
        auto gameObject = CreateElement(name);
        if (auto *rectTransform = gameObject->GetComponent<RectTransform>())
        {
            rectTransform->SetSizeDelta(Math::Vector2{160.0f, 20.0f});
        }
        if (auto *bar = gameObject->AddComponent<Image>())
        {
            bar->SetColor(Common::Color{0.25f, 0.25f, 0.25f, 1.0f});
        }
        auto *slider = gameObject->AddComponent<Slider>();

        auto fill = CreateElement("Fill");
        RectTransform *fillTransform = fill->GetComponent<RectTransform>();
        if (fillTransform)
        {
            fillTransform->StretchToParent();
        }
        if (auto *image = fill->AddComponent<Image>())
        {
            image->SetColor(Common::Color{0.45f, 0.75f, 1.0f, 1.0f});
        }
        gameObject->AddChild(fill, false);

        auto handle = CreateElement("Handle");
        RectTransform *handleTransform = handle->GetComponent<RectTransform>();
        if (handleTransform)
        {
            handleTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
            handleTransform->SetAnchorMax(Math::Vector2{0.0f, 1.0f});
            handleTransform->SetAnchoredPosition(Math::Vector2{0.0f, 0.0f});
            handleTransform->SetSizeDelta(Math::Vector2{16.0f, 0.0f});
        }
        handle->AddComponent<Image>();
        gameObject->AddChild(handle, false);

        if (slider)
        {
            slider->SetFillRect(fillTransform);
            slider->SetHandleRect(handleTransform);
        }
        return gameObject;
    }

    std::shared_ptr<GameObject> UISystem::CreateScrollView(const std::string &name, const bool vertical,
                                                           const bool horizontal)
    {
        auto gameObject = CreateElement(name);
        if (auto *rectTransform = gameObject->GetComponent<RectTransform>())
        {
            rectTransform->SetSizeDelta(Math::Vector2{300.0f, 200.0f});
        }
        if (auto *background = gameObject->AddComponent<Image>())
        {
            background->SetColor(Common::Color{0.1f, 0.1f, 0.1f, 0.8f});
        }
        gameObject->AddComponent<RectMask>();
        auto *scrollView = gameObject->AddComponent<ScrollView>();

        // The content, at the view's top-left
        auto content = CreateElement("Content");
        if (auto *rectTransform = content->GetComponent<RectTransform>())
        {
            rectTransform->SetAnchorMin(Math::Vector2{0.0f, vertical ? 1.0f : 0.0f});
            rectTransform->SetAnchorMax(Math::Vector2{horizontal ? 0.0f : 1.0f, 1.0f});
            rectTransform->SetPivot(Math::Vector2{0.0f, 1.0f});
            rectTransform->SetAnchoredPosition(Math::Vector2{0.0f, 0.0f});
            rectTransform->SetSizeDelta(Math::Vector2{horizontal ? 600.0f : 0.0f, vertical ? 400.0f : 0.0f});
            if (scrollView)
            {
                scrollView->SetContent(rectTransform);
            }
        }
        gameObject->AddChild(content, false);

        // A grey track along one edge with a lighter handle the ScrollView places
        auto addScrollbar = [&gameObject](const std::string &trackName, const bool isVertical) -> RectTransform *
        {
            auto track = CreateElement(trackName);
            if (auto *rectTransform = track->GetComponent<RectTransform>())
            {
                rectTransform->SetAnchorMin(Math::Vector2{isVertical ? 1.0f : 0.0f, 0.0f});
                rectTransform->SetAnchorMax(Math::Vector2{1.0f, isVertical ? 1.0f : 0.0f});
                rectTransform->SetPivot(Math::Vector2{isVertical ? 1.0f : 0.5f, isVertical ? 0.5f : 0.0f});
                rectTransform->SetAnchoredPosition(Math::Vector2{0.0f, 0.0f});
                rectTransform->SetSizeDelta(Math::Vector2{isVertical ? 10.0f : 0.0f, isVertical ? 0.0f : 10.0f});
            }
            if (auto *image = track->AddComponent<Image>())
            {
                image->SetColor(Common::Color{0.25f, 0.25f, 0.25f, 1.0f});
            }
            auto handle = CreateElement("Handle");
            RectTransform *handleTransform = handle->GetComponent<RectTransform>();
            if (handleTransform)
            {
                handleTransform->StretchToParent();
            }
            if (auto *image = handle->AddComponent<Image>())
            {
                image->SetColor(Common::Color{0.7f, 0.7f, 0.7f, 1.0f});
            }
            track->AddChild(handle, false);
            gameObject->AddChild(track, false);
            return handleTransform;
        };
        if (vertical && scrollView)
        {
            scrollView->SetVerticalScrollbar(addScrollbar("Scrollbar Vertical", true));
        }
        if (horizontal && scrollView)
        {
            scrollView->SetHorizontalScrollbar(addScrollbar("Scrollbar Horizontal", false));
        }
        if (scrollView)
        {
            scrollView->SetVertical(vertical);
            scrollView->SetHorizontal(horizontal);
        }
        return gameObject;
    }
}
