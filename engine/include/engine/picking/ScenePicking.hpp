#pragma once

#include <limits>
#include <memory>
#include <optional>

#include <math/Matrix.hpp>
#include <math/Ray.hpp>
#include <math/Vector3.hpp>

#include "engine/Camera.hpp" // BoundingBox

namespace N2Engine
{
    class GameObject;
    class Scene;
}

namespace N2Engine::Rendering
{
    class Mesh;
}

namespace N2Engine::Picking
{
    /**
     * Picking on the CPU, from a world-space ray, with no physics and no GPU: it works on a scene that was loaded
     * and never attached (the editor's edit mode, where no physics body exists yet) and on a headless host.
     *
     * What can be picked, per object (its renderables are what is drawn):
     * - Each active IRenderable with world bounds (IRenderable::GetWorldBounds) is a candidate. The ray is tested
     *   against the boxes, nearest first, then against the exact shape, so a ray passing through the empty corner
     *   of a box misses: triangles for a mesh (MeshRenderer and the built-in shapes, both sides), the laid-out
     *   rectangle for text, and, for a world-space canvas, the topmost graphic under the ray (the picked object is
     *   that graphic's, not the canvas's; a ray through an empty part of the canvas goes on to what is behind).
     *   A renderable of another kind is picked by its box.
     * - An object with a transform (a Positionable) and no shape of its own (an empty GameObject, a light, a
     *   camera, text with no text) is picked by a small sphere (PickSphereRadius) around its position,
     *   so it can be selected. Not a UI object (a canvas, or anything with a RectTransform): it has its rect. An object without a transform has no place in the world and is never picked.
     * - Inactive objects, and renderables whose component is disabled, are skipped unless includeInactive is set.
     *   Graphics inside a canvas follow the canvas's layout, which skips inactive objects either way.
     * - Overlay canvases are not part of the world: nothing of them is picked (the editor's view doesn't draw them).
     * The nearest exact hit wins; on a tie, the object earlier in hierarchy order.
     */
    inline constexpr float PickSphereRadius = 0.25f;

    struct PickOptions
    {
        /// Also pick objects that are inactive (or inactive in the hierarchy) and renderables that are disabled
        bool includeInactive = false;
        /// Hits farther along the ray than this are ignored
        float maxDistance = std::numeric_limits<float>::infinity();
    };

    struct PickHit
    {
        /// The object picked, or nullptr for a miss
        GameObject *gameObject = nullptr;
        /// Where the ray meets it, in the world
        Math::Vector3 point{0.0f, 0.0f, 0.0f};
        /// From the ray's origin to the point, in units of the ray's direction (world units for a unit direction)
        float distance = 0.0f;
    };

    /// The nearest object under `ray` in the scene's hierarchy (see above); gameObject is nullptr for none
    [[nodiscard]] PickHit PickGameObject(const Scene &scene, const Math::Ray &ray, const PickOptions &options = {});

    /**
     * The layouts of world-space canvases, kept for the calls that are given the same cache. Laying a canvas out is
     * the costly part of asking about its bounds, and every UI element under a canvas needs the layout of its
     * canvas: share one LayoutCache between the GetGameObjectBounds calls of one request so each canvas is laid
     * out once, not once per object. PickGameObject uses one of its own for each pick.
     *
     * A cache is only right while the scene does not change (no object added, moved, resized or switched, no
     * component changed): make one per request and drop it after. A new one is always empty.
     */
    class LayoutCache
    {
    public:
        LayoutCache();
        ~LayoutCache();
        LayoutCache(const LayoutCache &) = delete;
        LayoutCache &operator=(const LayoutCache &) = delete;

        struct Impl;
        [[nodiscard]] Impl &GetImpl() { return *_impl; }

    private:
        std::unique_ptr<Impl> _impl;
    };

    /**
     * The box, in world space, around an object for a selection box and "frame selected": the union of the
     * GetWorldBounds of the renderables on the object and on everything under it, plus the rect of each UI element
     * of a world-space canvas. Inactive descendants (and their subtrees) don't count, and the object itself counts
     * even when it is inactive, as it is the one asked about. With no such bounds, a box around the object's own
     * pick sphere; nullopt for an object without a transform and without bounds below it.
     */
    [[nodiscard]] std::optional<BoundingBox> GetGameObjectBounds(const GameObject &gameObject);
    /// The same, laying each canvas out at most once for every call given `cache` (see LayoutCache)
    [[nodiscard]] std::optional<BoundingBox> GetGameObjectBounds(const GameObject &gameObject, LayoutCache &cache);

    /// The pick sphere's box for an object at `position`
    [[nodiscard]] BoundingBox PickSphereBounds(const Math::Vector3 &position);

    // ==================== Ray tests (the pieces PickGameObject is made of) ====================
    // `ray.direction` need not be unit length: distances are in its units. A hit at the origin (the ray starts inside) is 0.

    /// The distance at which the ray enters the box (0 if it starts inside), within [0, maxDistance]
    [[nodiscard]] std::optional<float> RaycastBox(const Math::Ray &ray, const BoundingBox &box,
                                                  float maxDistance = std::numeric_limits<float>::infinity());

    /// The distance at which the ray enters the sphere (0 if it starts inside), within [0, maxDistance]
    [[nodiscard]] std::optional<float> RaycastSphere(const Math::Ray &ray, const Math::Vector3 &center, float radius,
                                                     float maxDistance = std::numeric_limits<float>::infinity());

    /**
     * The nearest triangle of the mesh, drawn with `model` (mesh space to world), under the ray, front or back
     * face. nullopt for a miss. It walks a bounding volume hierarchy of the mesh's triangles, built on the first
     * call for a mesh and kept with it (Mesh::GetCpuCache) until the mesh's data changes, so the cost of a pick is
     * about the log of the triangle count. The triangles are still tested in world space (no inverse of `model`, so a
     * flat or zero-scale model works), and the answer is exactly RaycastMeshLinear's.
     */
    [[nodiscard]] std::optional<float> RaycastMesh(const Rendering::Mesh &mesh, const Math::Matrix<float, 4, 4> &model,
                                                   const Math::Ray &ray,
                                                   float maxDistance = std::numeric_limits<float>::infinity());

    /// RaycastMesh by testing every triangle (of each submesh whose box the ray enters): the reference the BVH
    /// must agree with, and what the tests compare it to
    [[nodiscard]] std::optional<float> RaycastMeshLinear(const Rendering::Mesh &mesh,
                                                         const Math::Matrix<float, 4, 4> &model, const Math::Ray &ray,
                                                         float maxDistance = std::numeric_limits<float>::infinity());
}
