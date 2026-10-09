#include "engine/picking/ScenePicking.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
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
    struct LayoutCache::Impl
    {
        // Node-based, so a reference to a layout stays valid as others are added
        std::unordered_map<const UI::Canvas *, std::vector<UI::UIDrawItem>> layouts;
    };

    /// What the file's own helpers may reach of a LayoutCache
    struct LayoutCacheAccess
    {
        static auto &Layouts(LayoutCache &cache) { return cache._impl->layouts; }
    };

    PickStats &GetPickStats()
    {
        static PickStats stats;
        return stats;
    }

    LayoutCache::LayoutCache() : _impl(std::make_unique<Impl>()) {}
    LayoutCache::~LayoutCache() = default;

    namespace
    {
        using Matrix4 = Math::Matrix<float, 4, 4>;

        /// The layout of a world canvas, made on first ask and shared after
        const std::vector<UI::UIDrawItem> &LaidOut(LayoutCache &cache, const UI::Canvas &canvas)
        {
            auto &layouts = LayoutCacheAccess::Layouts(cache);
            if (const auto found = layouts.find(&canvas); found != layouts.end())
            {
                return found->second;
            }
            ++GetPickStats().canvasLayouts;
            return layouts.emplace(&canvas, UI::UISystem::CollectWorldCanvasGraphics(canvas)).first->second;
        }

        /// IRenderable::GetWorldBounds, but a canvas takes its bounds from the shared layout
        std::optional<BoundingBox> WorldBoundsOf(const IRenderable &renderable, LayoutCache &cache)
        {
            if (const auto *canvas = dynamic_cast<const UI::Canvas *>(&renderable))
            {
                return canvas->ComputeWorldBounds(LaidOut(cache, *canvas));
            }
            return renderable.GetWorldBounds();
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

        /// The world box of a mesh-space box under `model`, padded for the rounding of the transform of the triangle
        /// vertices inside it. The error of TransformPoint grows with the size of the terms it adds (a vertex far
        /// from the origin under a translation that brings it back), not with the result, so the pad is sized from
        /// sum |m_ij| * |corner_j| + |t_i| as well as from the result's own magnitude.
        BoundingBox TransformedPadded(const BoundingBox &local, const Matrix4 &model)
        {
            BoundingBox world = Padded(local.Transformed(model));
            float reach[3];
            for (int j = 0; j < 3; ++j)
            {
                reach[j] = std::max(std::abs(local.min[j]), std::abs(local.max[j]));
            }
            for (int i = 0; i < 3; ++i)
            {
                float terms = std::abs(model(i, 3));
                for (int j = 0; j < 3; ++j)
                {
                    terms += std::abs(model(i, j)) * reach[j];
                }
                const float pad = 1e-4f * terms;
                world.min[i] -= pad;
                world.max[i] += pad;
            }
            return world;
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

        using CanvasGraphics = std::vector<UI::UIDrawItem>;

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
        std::function<std::optional<ExactHit>(float)> CanvasTest(const UI::Canvas &canvas, const CanvasGraphics &graphics,
                                                                 const Math::Ray &ray)
        {
            // `graphics` lives in the pick's LayoutCache, which outlives the candidates
            return [&items = graphics,
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
                               LayoutCache &layouts, std::vector<Candidate> &out)
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
                const std::optional<BoundingBox> bounds = WorldBoundsOf(*renderable, layouts);
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
                                            ? CanvasTest(*canvas, LaidOut(layouts, *canvas), ray)
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
                    CollectCandidates(*child, ray, options, layouts, out);
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
                              std::optional<BoundingBox> &into, LayoutCache &layouts)
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
                if (const std::optional<BoundingBox> bounds = WorldBoundsOf(*renderable, layouts); bounds && Finite(*bounds))
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
                    (void)LaidOut(layouts, *canvas); // resolves every rect under it, once
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
                    AccumulateBounds(*child, false, rootActive, into, layouts);
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

    namespace
    {
        /// The triangles of a mesh in a bounding volume hierarchy, per submesh (so the submesh boxes still come first,
        /// as in RaycastMeshLinear). Built from the mesh space vertices and kept with the mesh until its data changes.
        /// A node's box is transformed by the model while walking: a conservative world box, so no inverse is needed.
        struct MeshBvh
        {
            using Triangle = std::array<std::uint32_t, 3>;

            struct Node
            {
                BoundingBox box;
                /// A leaf (count > 0) holds triangles [first, first + count); an inner node has its left child at the
                /// next index and its right child at `right`
                std::uint32_t first = 0;
                std::uint32_t count = 0;
                std::uint32_t right = 0;
            };

            struct Range
            {
                BoundingBox bounds;
                std::uint32_t root = 0;
                bool hasRoot = false;
                /// Triangles with a vertex that isn't finite have no box to sort by: they are always tested
                std::uint32_t looseFirst = 0;
                std::uint32_t looseCount = 0;
            };

            std::vector<Node> nodes;
            std::vector<Triangle> triangles;
            std::vector<Triangle> loose;
            std::vector<Range> ranges;
        };

        constexpr std::size_t BvhLeafSize = 4;
        constexpr std::size_t BvhStackSize = 64;
        // Split at the median, a tree over at most 2^32 triangles is at most 32 deep, and the walk holds at most one
        // pending sibling per level plus the one it is on
        static_assert(BvhStackSize >= 32 + 2, "the walk's stack must cover the deepest tree");

        struct BvhItem
        {
            BoundingBox box;
            Math::Vector3 centroid;
            MeshBvh::Triangle triangle;
        };

        std::uint32_t BuildNode(MeshBvh &bvh, std::vector<BvhItem> &items, const std::size_t begin, const std::size_t end)
        {
            const auto index = static_cast<std::uint32_t>(bvh.nodes.size());
            bvh.nodes.emplace_back();

            std::optional<BoundingBox> box;
            std::optional<BoundingBox> centroids;
            for (std::size_t i = begin; i < end; ++i)
            {
                Extend(box, items[i].box);
                Extend(centroids, BoundingBox{items[i].centroid, items[i].centroid});
            }
            bvh.nodes[index].box = *box;

            if (end - begin <= BvhLeafSize)
            {
                bvh.nodes[index].first = static_cast<std::uint32_t>(bvh.triangles.size());
                bvh.nodes[index].count = static_cast<std::uint32_t>(end - begin);
                for (std::size_t i = begin; i < end; ++i)
                {
                    bvh.triangles.push_back(items[i].triangle);
                }
                return index;
            }

            int axis = 0;
            float widest = -1.0f;
            for (int a = 0; a < 3; ++a)
            {
                const float extent = centroids->max[a] - centroids->min[a];
                if (extent > widest)
                {
                    widest = extent;
                    axis = a;
                }
            }
            const std::size_t middle = begin + (end - begin) / 2;
            std::nth_element(items.begin() + static_cast<std::ptrdiff_t>(begin),
                             items.begin() + static_cast<std::ptrdiff_t>(middle),
                             items.begin() + static_cast<std::ptrdiff_t>(end),
                             [axis](const BvhItem &a, const BvhItem &b) { return a.centroid[axis] < b.centroid[axis]; });
            BuildNode(bvh, items, begin, middle); // the left child is the next node
            const std::uint32_t right = BuildNode(bvh, items, middle, end);
            bvh.nodes[index].right = right;
            return index;
        }

        std::shared_ptr<const MeshBvh> BuildMeshBvh(const Rendering::Mesh &mesh)
        {
            auto bvh = std::make_shared<MeshBvh>();
            const auto &vertices = mesh.GetVertices();
            const auto &indices = mesh.GetIndices();

            struct Source
            {
                std::size_t first;
                std::size_t count;
                BoundingBox bounds;
            };
            std::vector<Source> sources;
            for (const Rendering::Submesh &submesh : mesh.GetSubmeshes())
            {
                sources.push_back(Source{submesh.firstIndex, submesh.indexCount, submesh.bounds});
            }
            if (sources.empty())
            {
                sources.push_back(Source{0, indices.size(), mesh.GetBounds()});
            }

            for (const Source &source : sources)
            {
                MeshBvh::Range range;
                range.bounds = source.bounds;
                range.looseFirst = static_cast<std::uint32_t>(bvh->loose.size());
                std::vector<BvhItem> items;
                const std::size_t end = std::min(source.first + source.count, indices.size());
                for (std::size_t i = source.first; i + 3 <= end; i += 3)
                {
                    const MeshBvh::Triangle triangle{indices[i], indices[i + 1], indices[i + 2]};
                    if (triangle[0] >= vertices.size() || triangle[1] >= vertices.size() ||
                        triangle[2] >= vertices.size())
                    {
                        continue;
                    }
                    const Math::Vector3 p0 = VertexPosition(vertices[triangle[0]]);
                    const Math::Vector3 p1 = VertexPosition(vertices[triangle[1]]);
                    const Math::Vector3 p2 = VertexPosition(vertices[triangle[2]]);
                    std::optional<BoundingBox> box;
                    Extend(box, BoundingBox{p0, p0});
                    Extend(box, BoundingBox{p1, p1});
                    Extend(box, BoundingBox{p2, p2});
                    if (!Finite(*box))
                    {
                        bvh->loose.push_back(triangle);
                        continue;
                    }
                    items.push_back(BvhItem{*box, Math::Vector3{(p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f, (p0.z + p1.z + p2.z) / 3.0f}, triangle});
                }
                range.looseCount = static_cast<std::uint32_t>(bvh->loose.size()) - range.looseFirst;
                if (!items.empty())
                {
                    range.root = BuildNode(*bvh, items, 0, items.size());
                    range.hasRoot = true;
                }
                bvh->ranges.push_back(range);
            }
            return bvh;
        }

        std::shared_ptr<const MeshBvh> BvhOf(const Rendering::Mesh &mesh)
        {
            if (std::shared_ptr<const MeshBvh> cached = mesh.GetCpuCache<MeshBvh>())
            {
                return cached;
            }
            std::shared_ptr<const MeshBvh> bvh = BuildMeshBvh(mesh);
            mesh.SetCpuCache(bvh);
            return bvh;
        }
    }

    std::optional<float> RaycastMesh(const Rendering::Mesh &mesh, const Math::Matrix<float, 4, 4> &model,
                                     const Math::Ray &ray, const float maxDistance)
    {
        if (!FiniteRay(ray))
        {
            return std::nullopt;
        }
        const std::shared_ptr<const MeshBvh> bvh = BvhOf(mesh);
        const auto &vertices = mesh.GetVertices();

        std::optional<float> best;
        float limit = maxDistance;
        PickStats &stats = GetPickStats();
        const auto test = [&](const MeshBvh::Triangle &triangle)
        {
            ++stats.bvhTrianglesTested;
            const std::optional<float> t =
                TriangleHit(ray.origin, ray.direction, model.TransformPoint(VertexPosition(vertices[triangle[0]])),
                            model.TransformPoint(VertexPosition(vertices[triangle[1]])),
                            model.TransformPoint(VertexPosition(vertices[triangle[2]])));
            if (t && *t <= limit)
            {
                best = t;
                limit = *t; // only nearer ones from here
            }
        };
        const auto entryOf = [&](const BoundingBox &box)
        { return BoxEntry(ray.origin, ray.direction, TransformedPadded(box, model), limit); };

        struct Pending
        {
            std::uint32_t node;
            float entry;
        };
        std::array<Pending, BvhStackSize> stack{};
        for (const MeshBvh::Range &range : bvh->ranges)
        {
            // In world space throughout: no inverse of the model, which fails for a flat or tiny one
            if (!entryOf(range.bounds))
            {
                continue;
            }
            for (std::uint32_t i = 0; i < range.looseCount; ++i)
            {
                test(bvh->loose[range.looseFirst + i]);
            }
            if (!range.hasRoot)
            {
                continue;
            }
            std::size_t top = 0;
            stack[top++] = Pending{range.root, 0.0f};
            while (top > 0)
            {
                const Pending pending = stack[--top];
                if (pending.entry > limit)
                {
                    continue; // found something nearer since this was pushed
                }
                const MeshBvh::Node &node = bvh->nodes[pending.node];
                ++stats.bvhNodesVisited;
                if (node.count > 0)
                {
                    for (std::uint32_t i = 0; i < node.count; ++i)
                    {
                        test(bvh->triangles[node.first + i]);
                    }
                    continue;
                }
                const std::uint32_t left = pending.node + 1;
                const std::uint32_t right = node.right;
                const std::optional<float> leftEntry = entryOf(bvh->nodes[left].box);
                const std::optional<float> rightEntry = entryOf(bvh->nodes[right].box);
                // The nearer child is popped first
                if (leftEntry && rightEntry && *rightEntry < *leftEntry)
                {
                    stack[top++] = Pending{left, *leftEntry};
                    stack[top++] = Pending{right, *rightEntry};
                }
                else
                {
                    if (rightEntry)
                    {
                        stack[top++] = Pending{right, *rightEntry};
                    }
                    if (leftEntry)
                    {
                        stack[top++] = Pending{left, *leftEntry};
                    }
                }
            }
        }
        return best;
    }

    std::optional<float> RaycastMeshLinear(const Rendering::Mesh &mesh, const Math::Matrix<float, 4, 4> &model,
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
            if (!BoxEntry(ray.origin, ray.direction, TransformedPadded(range.bounds, model), limit))
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
        // Declared first, so it outlives the candidates, whose canvas tests refer to its layouts
        LayoutCache layouts; // each canvas laid out once for this pick
        std::vector<Candidate> candidates;
        for (const auto &root : scene.GetRootGameObjects())
        {
            if (root)
            {
                CollectCandidates(*root, ray, options, layouts, candidates);
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
        LayoutCache layouts;
        return GetGameObjectBounds(gameObject, layouts);
    }

    std::optional<BoundingBox> GetGameObjectBounds(const GameObject &gameObject, LayoutCache &layouts)
    {
        std::optional<BoundingBox> bounds;
        AccumulateBounds(gameObject, true, gameObject.IsActiveInHierarchy(), bounds, layouts);
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
