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
#include "engine/ui/UISystem.hpp"

namespace N2Engine::Picking
{
    namespace
    {
        using Matrix4 = Math::Matrix<float, 4, 4>;

        /// A ray in an object's own space. The direction is not unit length (it carries the object's scale), so a
        /// distance along it is the same number as along the world ray.
        struct LocalRay
        {
            Math::Vector3 origin;
            Math::Vector3 direction;
        };

        Math::Vector3 TransformDirection(const Matrix4 &m, const Math::Vector3 &v)
        {
            return Math::Vector3{m(0, 0) * v.x + m(0, 1) * v.y + m(0, 2) * v.z,
                                 m(1, 0) * v.x + m(1, 1) * v.y + m(1, 2) * v.z,
                                 m(2, 0) * v.x + m(2, 1) * v.y + m(2, 2) * v.z};
        }

        /// The ray in the space `model` maps from; nullopt when the model can't be inverted
        std::optional<LocalRay> ToLocal(const Matrix4 &model, const Math::Ray &ray)
        {
            try
            {
                const Matrix4 inverse = model.inverse();
                const LocalRay local{inverse.TransformPoint(ray.origin), TransformDirection(inverse, ray.direction)};
                if (!std::isfinite(local.origin.x) || !std::isfinite(local.origin.y) || !std::isfinite(local.origin.z) ||
                    !std::isfinite(local.direction.x) || !std::isfinite(local.direction.y) ||
                    !std::isfinite(local.direction.z))
                {
                    return std::nullopt;
                }
                return local;
            }
            catch (const std::runtime_error &)
            {
                return std::nullopt; // singular: a zero scale
            }
        }

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

        /// A rectangle on the object's z = 0 plane (local space), under the ray
        std::optional<float> RectHit(const Matrix4 &model, const Text::Rect &rect, const Math::Ray &ray,
                                     const float maxDistance)
        {
            const std::optional<LocalRay> local = ToLocal(model, ray);
            if (!local || std::abs(local->direction.z) < 1e-12f)
            {
                return std::nullopt;
            }
            const float t = -local->origin.z / local->direction.z;
            if (!(t >= 0.0f) || t > maxDistance)
            {
                return std::nullopt;
            }
            const float x = local->origin.x + t * local->direction.x;
            const float y = local->origin.y + t * local->direction.y;
            if (x < rect.minX || x > rect.maxX || y < rect.minY || y > rect.maxY)
            {
                return std::nullopt;
            }
            return t;
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
        std::function<std::optional<ExactHit>(float)> CanvasTest(const UI::Canvas &canvas, const Math::Ray &ray)
        {
            return [items = UI::UISystem::CollectWorldCanvasGraphics(canvas),
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
                const auto *canvas = dynamic_cast<const UI::Canvas *>(renderable);
                if (canvas == nullptr)
                {
                    hasShape = true;
                }
                if (!entry)
                {
                    continue;
                }
                out.push_back(Candidate{*entry, canvas != nullptr ? CanvasTest(*canvas, ray)
                                                                  : ExactTestFor(*renderable, ray, *bounds)});
            }

            // No shape of its own (an empty object, a light, a camera, a canvas): a sphere at its position
            if (!hasShape)
            {
                if (const Positionable *positionable = gameObject.GetPositionable())
                {
                    const Math::Vector3 center = positionable->GetPosition();
                    if (const auto entry = RaycastBox(ray, PickSphereBounds(center), options.maxDistance))
                    {
                        GameObject *owner = &gameObject;
                        out.push_back(Candidate{*entry, [owner, center, ray](const float limit)
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
                if (renderable == nullptr || (rootActive && !renderable->IsActive()))
                {
                    continue;
                }
                if (const std::optional<BoundingBox> bounds = renderable->GetWorldBounds(); bounds && Finite(*bounds))
                {
                    Extend(into, *bounds);
                }
            }

            // A UI element of a world-space canvas: its rect, as the layout last resolved it
            if (const UI::RectTransform *rectTransform = gameObject.GetComponent<UI::RectTransform>())
            {
                if (const UI::Canvas *canvas = WorldCanvasAbove(gameObject))
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

    std::optional<float> RaycastBox(const Math::Ray &ray, const BoundingBox &box, const float maxDistance)
    {
        return BoxEntry(ray.origin, ray.direction, box, maxDistance);
    }

    std::optional<float> RaycastSphere(const Math::Ray &ray, const Math::Vector3 &center, const float radius,
                                       const float maxDistance)
    {
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
        const std::optional<LocalRay> local = ToLocal(model, ray);
        if (!local)
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
            if (!BoxEntry(local->origin, local->direction, Padded(range.bounds), limit))
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
                const std::optional<float> t = TriangleHit(local->origin, local->direction, VertexPosition(vertices[a]),
                                                           VertexPosition(vertices[b]), VertexPosition(vertices[c]));
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
        std::ranges::stable_sort(candidates, [](const Candidate &a, const Candidate &b) { return a.entry < b.entry; });

        PickHit best;
        float limit = options.maxDistance;
        for (const Candidate &candidate : candidates)
        {
            if (candidate.entry > limit)
            {
                break; // every later box is entered farther away than the hit already found
            }
            const std::optional<ExactHit> hit = candidate.exact(limit);
            if (hit && hit->gameObject != nullptr && (best.gameObject == nullptr || hit->distance < best.distance))
            {
                best.gameObject = hit->gameObject;
                best.distance = hit->distance;
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
        if (const Positionable *positionable = gameObject.GetPositionable())
        {
            return PickSphereBounds(positionable->GetPosition());
        }
        return std::nullopt;
    }
}
