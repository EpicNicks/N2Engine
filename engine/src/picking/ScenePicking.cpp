#include "engine/picking/ScenePicking.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/example/renderers/PolygonRenderer.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIGraphic.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::Picking
{
    namespace
    {
        using Matrix4 = Math::Matrix<float, 4, 4>;

        std::optional<float> BoxEntry(const Math::Vector3 &origin, const Math::Vector3 &direction,
                                      const BoundingBox &box, const float maxDistance)
        {
            float tNear = 0.0f;
            float tFar = maxDistance;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float o = origin[axis];
                const float d = direction[axis];
                const float lo = std::min(box.min[axis], box.max[axis]);
                const float hi = std::max(box.min[axis], box.max[axis]);
                if (std::abs(d) < 1e-12f)
                {
                    // Parallel to this pair of faces: inside their slab or never
                    if (o < lo || o > hi)
                    {
                        return std::nullopt;
                    }
                    continue;
                }
                float t0 = (lo - o) / d;
                float t1 = (hi - o) / d;
                if (t0 > t1)
                {
                    std::swap(t0, t1);
                }
                tNear = std::max(tNear, t0);
                tFar = std::min(tFar, t1);
                if (tNear > tFar)
                {
                    return std::nullopt;
                }
            }
            return tNear;
        }

        /// Moller-Trumbore, both faces. The distance along the ray's direction, or nullopt.
        std::optional<float> TriangleHit(const Math::Vector3 &origin, const Math::Vector3 &direction,
                                         const Math::Vector3 &v0, const Math::Vector3 &v1, const Math::Vector3 &v2)
        {
            constexpr float edgeTolerance = 1e-6f; // so two triangles sharing an edge leave no crack
            const Math::Vector3 e1 = v1 - v0;
            const Math::Vector3 e2 = v2 - v0;
            const Math::Vector3 p = direction.Cross(e2);
            const float determinant = e1.Dot(p);
            if (std::abs(determinant) < 1e-20f)
            {
                return std::nullopt; // parallel to the triangle, or a degenerate one
            }
            const float inverse = 1.0f / determinant;
            const Math::Vector3 s = origin - v0;
            const float u = s.Dot(p) * inverse;
            if (u < -edgeTolerance || u > 1.0f + edgeTolerance)
            {
                return std::nullopt;
            }
            const Math::Vector3 q = s.Cross(e1);
            const float v = direction.Dot(q) * inverse;
            if (v < -edgeTolerance || u + v > 1.0f + edgeTolerance)
            {
                return std::nullopt;
            }
            const float t = e2.Dot(q) * inverse;
            if (!(t >= 0.0f) || !std::isfinite(t))
            {
                return std::nullopt;
            }
            return t;
        }

        Math::Vector3 VertexPosition(const Renderer::Common::Vertex &vertex)
        {
            return Math::Vector3{vertex.position[0], vertex.position[1], vertex.position[2]};
        }

        /// A rectangle on the object's z = 0 plane (local space), moved by `model`, under the ray: its two triangles
        /// in world space, so a flat or tiny model (a zero or small scale) needs no inverse
        std::optional<float> RectHit(const Matrix4 &model, const Text::Rect &rect, const Math::Ray &ray,
                                     const float maxDistance)
        {
            const Math::Vector3 c00 = model.TransformPoint(Math::Vector3{rect.minX, rect.minY, 0.0f});
            const Math::Vector3 c10 = model.TransformPoint(Math::Vector3{rect.maxX, rect.minY, 0.0f});
            const Math::Vector3 c11 = model.TransformPoint(Math::Vector3{rect.maxX, rect.maxY, 0.0f});
            const Math::Vector3 c01 = model.TransformPoint(Math::Vector3{rect.minX, rect.maxY, 0.0f});
            std::optional<float> best;
            for (const std::optional<float> t : {TriangleHit(ray.origin, ray.direction, c00, c10, c11),
                                                 TriangleHit(ray.origin, ray.direction, c00, c11, c01)})
            {
                if (t && *t <= maxDistance && (!best || *t < *best))
                {
                    best = t;
                }
            }
            return best;
        }

        BoundingBox Padded(const BoundingBox &box)
        {
            // The box tests only order the candidates; a flat box (text, a quad) or rounding must not drop a true hit
            const auto pad = [](const float lo, const float hi)
            { return 1e-4f * std::max(1.0f, std::max(std::abs(lo), std::abs(hi))); };
            const Math::Vector3 e{pad(box.min.x, box.max.x), pad(box.min.y, box.max.y), pad(box.min.z, box.max.z)};
            return BoundingBox{box.min - e, box.max + e};
        }

        void Extend(std::optional<BoundingBox> &into, const BoundingBox &box)
        {
            if (!into)
            {
                into = box;
                return;
            }
            into->min = Math::Vector3{std::min(into->min.x, box.min.x), std::min(into->min.y, box.min.y),
                                      std::min(into->min.z, box.min.z)};
            into->max = Math::Vector3{std::max(into->max.x, box.max.x), std::max(into->max.y, box.max.y),
                                      std::max(into->max.z, box.max.z)};
        }

        bool Finite(const BoundingBox &box)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(box.min[axis]) || !std::isfinite(box.max[axis]))
                {
                    return false;
                }
            }
            return true;
        }

        /// A UI object (a canvas, or an element with a RectTransform) has its rect, not a place of its own in the
        /// world, so it never gets a pick sphere (even when the editor gave it a transform)
        bool IsUiObject(const GameObject &gameObject)
        {
            return gameObject.GetComponent<UI::RectTransform>() != nullptr || gameObject.GetComponent<UI::Canvas>() != nullptr;
        }

        /// What an exact test found
        struct ExactHit
        {
            GameObject *gameObject = nullptr;
            float distance = 0.0f;
        };

        struct Candidate
        {
            /// Where the ray enters the (padded) box: nothing exact can be nearer
            float entry = 0.0f;
            /// Where it was collected: hierarchy order, which breaks a tie in distance
            std::size_t order = 0;
            /// The exact test, within the distance given; nullopt for a miss
            std::function<std::optional<ExactHit>(float)> exact;
        };

        /// The shape-specific test of one renderable
        std::function<std::optional<ExactHit>(float)> ExactTestFor(IRenderable &renderable, const Math::Ray &ray,
                                                                   const BoundingBox &box)
        {
            GameObject *owner = &renderable.GetGameObject();

            if (auto *polygon = dynamic_cast<Example::PolygonRenderer *>(&renderable))
            {
                const std::optional<Matrix4> model = polygon->GetModelMatrix();
                std::shared_ptr<Rendering::Mesh> mesh = polygon->GetMesh();
                if (model && mesh)
                {
                    return [owner, model = *model, mesh = std::move(mesh), ray](const float limit)
                    {
                        const std::optional<float> t = RaycastMesh(*mesh, model, ray, limit);
                        return t ? std::optional<ExactHit>{ExactHit{owner, *t}} : std::nullopt;
                    };
                }
            }
            if (auto *meshRenderer = dynamic_cast<Rendering::MeshRenderer *>(&renderable))
            {
                const Positionable *positionable = owner->GetPositionable();
                if (meshRenderer->GetMesh() && positionable)
                {
                    return [owner, model = positionable->GetLocalToWorldMatrix(), mesh = meshRenderer->GetMesh(),
                            ray](const float limit)
                    {
                        const std::optional<float> t = RaycastMesh(*mesh, model, ray, limit);
                        return t ? std::optional<ExactHit>{ExactHit{owner, *t}} : std::nullopt;
                    };
                }
            }
            if (auto *text = dynamic_cast<Rendering::TextRenderer *>(&renderable))
            {
                const Positionable *positionable = owner->GetPositionable();
                if (positionable)
                {
                    return [owner, model = positionable->GetLocalToWorldMatrix(), rect = text->GetLayout().bounds,
                            ray](const float limit)
                    {
                        const std::optional<float> t = RectHit(model, rect, ray, limit);
                        return t ? std::optional<ExactHit>{ExactHit{owner, *t}} : std::nullopt;
                    };
                }
            }
            // Anything else is picked by its box
            return [owner, box, ray](const float limit)
            {
                const std::optional<float> t = RaycastBox(ray, box, limit);
                return t ? std::optional<ExactHit>{ExactHit{owner, *t}} : std::nullopt;
            };
        }

        /// A world-space canvas: the topmost graphic under the ray (any graphic, raycast target or not: the editor
        /// picks what it sees), or a miss
        std::function<std::optional<ExactHit>(float)> CanvasTest(const UI::Canvas &canvas,
                                                                 std::vector<UI::UIDrawItem> graphics,
                                                                 const Math::Ray &ray)
        {
            return [items = std::move(graphics),
                    canvasToWorld = canvas.GetCanvasToWorldMatrix(), ray](const float limit)
                -> std::optional<ExactHit>
            {
                const std::optional<UI::CanvasRayHit> plane = UI::UISystem::RaycastCanvasPlane(canvasToWorld, ray, limit);
                if (!plane)
                {
                    return std::nullopt;
                }
                for (auto it = items.rbegin(); it != items.rend(); ++it)
                {
                    if (it->rect.Contains(plane->canvasPoint))
                    {
                        return ExactHit{&it->graphic->GetGameObject(), plane->distance};
                    }
                }
                return std::nullopt;
            };
        }

        void CollectCandidates(GameObject &gameObject, const Math::Ray &ray, const PickOptions &options,
                               std::vector<Candidate> &out)
        {
            if (!options.includeInactive && !gameObject.IsActiveInHierarchy())
            {
                return;
            }

            bool hasShape = false;
            for (IRenderable *renderable : gameObject.GetComponents<IRenderable>())
            {
                if (renderable == nullptr || (!options.includeInactive && !renderable->IsActive()))
                {
                    continue;
                }
                const std::optional<BoundingBox> bounds = renderable->GetWorldBounds();
                if (!bounds || !Finite(*bounds))
                {
                    continue;
                }
                const std::optional<float> entry = RaycastBox(ray, Padded(*bounds), options.maxDistance);
                // A world canvas with bounds is a shape too: its graphics are what is clicked, and a sphere at its
                // centre would cover them
                hasShape = true;
                if (!entry)
                {
                    continue;
                }
                const auto *canvas = dynamic_cast<const UI::Canvas *>(renderable);
                out.push_back(Candidate{*entry, 0,
                                        canvas != nullptr
                                            ? CanvasTest(*canvas, UI::UISystem::CollectWorldCanvasGraphics(*canvas), ray)
                                            : ExactTestFor(*renderable, ray, *bounds)});
            }

            // No shape of its own (an empty object, a light, a camera): a sphere at its position. Not for a UI object.
            if (!hasShape && !IsUiObject(gameObject))
            {
                if (const Positionable *positionable = gameObject.GetPositionable())
                {
                    const Math::Vector3 center = positionable->GetPosition();
                    if (const auto entry = RaycastBox(ray, PickSphereBounds(center), options.maxDistance))
                    {
                        GameObject *owner = &gameObject;
                        out.push_back(Candidate{*entry, 0, [owner, center, ray](const float limit)
                        {
                            const std::optional<float> t = RaycastSphere(ray, center, PickSphereRadius, limit);
                            return t ? std::optional<ExactHit>{ExactHit{owner, *t}} : std::nullopt;
                        }});
                    }
                }
            }

            for (const auto &child : gameObject.GetChildren())
            {
                if (child)
                {
                    CollectCandidates(*child, ray, options, out);
                }
            }
        }

        /// The world-space root canvas a UI element is under, or nullptr (also for the canvas object itself)
        const UI::Canvas *WorldCanvasAbove(const GameObject &gameObject)
        {
            GameObject::Ptr top;
            for (GameObject::Ptr parent = gameObject.GetParent(); parent; parent = parent->GetParent())
            {
                if (parent->HasComponent<UI::Canvas>())
                {
                    top = parent;
                }
            }
            if (!top)
            {
                return nullptr;
            }
            for (const UI::Canvas *canvas : top->GetComponents<UI::Canvas>())
            {
                if (canvas != nullptr && canvas->IsWorldSpace() && canvas->IsRootCanvas())
                {
                    return canvas;
                }
            }
            return nullptr;
        }

        /// The bounds of the shapes on `gameObject` and under it. `isRoot` is the object asked about.
        void AccumulateBounds(const GameObject &gameObject, const bool isRoot, const bool rootActive,
                              std::optional<BoundingBox> &into, std::vector<const UI::Canvas *> &laidOut)
        {
            // Descendants that are switched off aren't drawn (and neither is what is under them)
            if (!isRoot && !gameObject.IsActive())
            {
                return;
            }
            for (const IRenderable *renderable : gameObject.GetComponents<IRenderable>())
            {
                if (renderable == nullptr || !renderable->IsActiveSelf())
                {
                    continue;
                }
                if (const std::optional<BoundingBox> bounds = renderable->GetWorldBounds(); bounds && Finite(*bounds))
                {
                    Extend(into, *bounds);
                }
            }

            // A UI element of a world-space canvas: its rect, as the layout last resolved it
            // (Not under an inactive canvas or object: the layout skips those, so their rects are stale.)
            if (const UI::RectTransform *rectTransform = gameObject.GetComponent<UI::RectTransform>();
                rectTransform != nullptr && rootActive)
            {
                if (const UI::Canvas *canvas = WorldCanvasAbove(gameObject); canvas != nullptr && canvas->IsActive())
                {
                    if (std::ranges::find(laidOut, canvas) == laidOut.end())
                    {
                        (void)UI::UISystem::CollectWorldCanvasGraphics(*canvas); // resolves every rect under it
                        laidOut.push_back(canvas);
                    }
                    const UI::Rect &rect = rectTransform->GetRect();
                    if (rect.HasArea())
                    {
                        const BoundingBox local{Math::Vector3{rect.XMin(), rect.YMin(), 0.0f},
                                                Math::Vector3{rect.XMax(), rect.YMax(), 0.0f}};
                        const BoundingBox world = local.Transformed(canvas->GetCanvasToWorldMatrix());
                        if (Finite(world))
                        {
                            Extend(into, world);
                        }
                    }
                }
            }

            for (const auto &child : gameObject.GetChildren())
            {
                if (child)
                {
                    AccumulateBounds(*child, false, rootActive, into, laidOut);
                }
            }
        }
    }

    BoundingBox PickSphereBounds(const Math::Vector3 &position)
    {
        const Math::Vector3 r{PickSphereRadius, PickSphereRadius, PickSphereRadius};
        return BoundingBox{position - r, position + r};
    }

    namespace
    {
        bool FiniteRay(const Math::Ray &ray)
        {
            return std::isfinite(ray.origin.x) && std::isfinite(ray.origin.y) && std::isfinite(ray.origin.z) &&
                   std::isfinite(ray.direction.x) && std::isfinite(ray.direction.y) && std::isfinite(ray.direction.z);
        }
    }

    std::optional<float> RaycastBox(const Math::Ray &ray, const BoundingBox &box, const float maxDistance)
    {
        if (!FiniteRay(ray))
        {
            return std::nullopt;
        }
        return BoxEntry(ray.origin, ray.direction, box, maxDistance);
    }

    std::optional<float> RaycastSphere(const Math::Ray &ray, const Math::Vector3 &center, const float radius,
                                       const float maxDistance)
    {
        if (!FiniteRay(ray))
        {
            return std::nullopt;
        }
        const Math::Vector3 toOrigin = ray.origin - center;
        const float a = ray.direction.Dot(ray.direction);
        if (!(a > 0.0f))
        {
            return std::nullopt;
        }
        const float halfB = toOrigin.Dot(ray.direction);
        const float c = toOrigin.Dot(toOrigin) - radius * radius;
        const float discriminant = halfB * halfB - a * c;
        if (!(discriminant >= 0.0f))
        {
            return std::nullopt;
        }
        const float root = std::sqrt(discriminant);
        const float tExit = (-halfB + root) / a;
        if (tExit < 0.0f)
        {
            return std::nullopt; // behind the ray
        }
        const float t = std::max(0.0f, (-halfB - root) / a);
        if (t > maxDistance)
        {
            return std::nullopt;
        }
        return t;
    }

    std::optional<float> RaycastMesh(const Rendering::Mesh &mesh, const Math::Matrix<float, 4, 4> &model,
                                     const Math::Ray &ray, const float maxDistance)
    {
        if (!FiniteRay(ray))
        {
            return std::nullopt;
        }
        const auto &vertices = mesh.GetVertices();
        const auto &indices = mesh.GetIndices();

        // Each submesh's box first, then its triangles
        struct Range
        {
            std::size_t first;
            std::size_t count;
            BoundingBox bounds;
        };
        std::vector<Range> ranges;
        for (const Rendering::Submesh &submesh : mesh.GetSubmeshes())
        {
            ranges.push_back(Range{submesh.firstIndex, submesh.indexCount, submesh.bounds});
        }
        if (ranges.empty())
        {
            ranges.push_back(Range{0, indices.size(), mesh.GetBounds()});
        }

        std::optional<float> best;
        float limit = maxDistance;
        for (const Range &range : ranges)
        {
            // In world space throughout: no inverse of the model, which fails for a flat or tiny one
            if (!BoxEntry(ray.origin, ray.direction, Padded(range.bounds.Transformed(model)), limit))
            {
                continue;
            }
            const std::size_t end = std::min(range.first + range.count, indices.size());
            for (std::size_t i = range.first; i + 3 <= end; i += 3)
            {
                const std::uint32_t a = indices[i];
                const std::uint32_t b = indices[i + 1];
                const std::uint32_t c = indices[i + 2];
                if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
                {
                    continue;
                }
                const std::optional<float> t =
                    TriangleHit(ray.origin, ray.direction, model.TransformPoint(VertexPosition(vertices[a])),
                                model.TransformPoint(VertexPosition(vertices[b])),
                                model.TransformPoint(VertexPosition(vertices[c])));
                if (t && *t <= limit)
                {
                    best = t;
                    limit = *t; // only nearer ones from here
                }
            }
        }
        return best;
    }

    PickHit PickGameObject(const Scene &scene, const Math::Ray &ray, const PickOptions &options)
    {
        std::vector<Candidate> candidates;
        for (const auto &root : scene.GetRootGameObjects())
        {
            if (root)
            {
                CollectCandidates(*root, ray, options, candidates);
            }
        }
        // Nearest box first (ties keep hierarchy order)
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            candidates[i].order = i;
        }
        std::ranges::stable_sort(candidates, [](const Candidate &a, const Candidate &b) { return a.entry < b.entry; });

        PickHit best;
        std::size_t bestOrder = 0;
        float limit = options.maxDistance;
        for (const Candidate &candidate : candidates)
        {
            if (candidate.entry > limit)
            {
                break; // every later box is entered farther away than the hit already found
            }
            const std::optional<ExactHit> hit = candidate.exact(limit);
            // Nearer wins; at the same distance the object collected first (earlier in hierarchy order)
            if (hit && hit->gameObject != nullptr &&
                (best.gameObject == nullptr || hit->distance < best.distance ||
                 (hit->distance == best.distance && candidate.order < bestOrder)))
            {
                best.gameObject = hit->gameObject;
                best.distance = hit->distance;
                bestOrder = candidate.order;
                limit = hit->distance;
            }
        }
        if (best.gameObject != nullptr)
        {
            best.point = ray.GetPoint(best.distance);
        }
        return best;
    }

    std::optional<BoundingBox> GetGameObjectBounds(const GameObject &gameObject)
    {
        std::optional<BoundingBox> bounds;
        std::vector<const UI::Canvas *> laidOut;
        AccumulateBounds(gameObject, true, gameObject.IsActiveInHierarchy(), bounds, laidOut);
        if (bounds)
        {
            return bounds;
        }
        // A UI object has its rect (above), not a place in the world: no sphere for it
        if (const Positionable *positionable = gameObject.GetPositionable();
            positionable != nullptr && !IsUiObject(gameObject))
        {
            return PickSphereBounds(positionable->GetPosition());
        }
        return std::nullopt;
    }
}
