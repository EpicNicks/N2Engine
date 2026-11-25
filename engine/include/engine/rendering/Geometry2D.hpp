#pragma once

// Flat geometry built on the CPU for LineRenderer, TrailRenderer and MeshRenderer2D (docs/meshes.html#lines): the line mesh (joins,
// caps, loops), polygon triangulation and the checks on a mesh a script builds. Pure functions, tested without a renderer, plus the
// draw path the three components share: one mesh per component, an unlit, double-sided, blended material, the Transparent queue.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Rendering
{
    /// How a line, trail or 2D mesh combines with what is behind it: Material's BlendMode Alpha or Additive, through the same blend
    /// state every blended draw uses (MeshDrawing::ApplyBlendMode). The software renderer cannot blend: see Geometry2DDraw.
    enum class Blend2D : std::uint8_t
    {
        Alpha,
        Additive
    };

    /// How a line turns a corner
    enum class LineJoin : std::uint8_t
    {
        /// Sharp corners: the two edges meet at a point, unless that point is further than the miter limit times half the width
        /// from the line, when the corner is bevelled instead
        Miter,
        /// Every corner cut flat across its outer side
        Bevel
    };

    /// How an open line ends
    enum class LineCap : std::uint8_t
    {
        /// Flat, at the end point
        Butt,
        /// Flat, half the width past the end point
        Square
    };

    // Literals as values, so renaming a constant does not change what scenes save
    N2_SERIALIZE_ENUM(Blend2D, {
                      {Blend2D::Alpha, "Alpha"},
                      {Blend2D::Additive, "Additive"}
                      })
    N2_SERIALIZE_ENUM(LineJoin, {
                      {LineJoin::Miter, "Miter"},
                      {LineJoin::Bevel, "Bevel"}
                      })
    N2_SERIALIZE_ENUM(LineCap, {
                      {LineCap::Butt, "Butt"},
                      {LineCap::Square, "Square"}
                      })

    /// The most points a LineRenderer takes (a longer list is refused) and a TrailRenderer keeps (its oldest go first)
    inline constexpr std::size_t kMaxLinePoints = 16384;
    inline constexpr std::size_t kMaxTrailPoints = 4096;
    /// The most vertices and indices a script's mesh may have (Mesh.Create, ProceduralMesh:SetData)
    inline constexpr std::size_t kMaxScriptMeshVertices = 65536;
    inline constexpr std::size_t kMaxScriptMeshIndices = 3 * 65536;
    /// The most points Mesh.Triangulate (TriangulatePolygon2D) takes
    inline constexpr std::size_t kMaxTriangulatePoints = 1024;

    /// One point of a line: where it is, how wide the line is there (world units, in the line's space) and its colour there
    struct LinePoint
    {
        Math::Vector3 position;
        float width = 0.1f;
        Common::Color color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct LineShape
    {
        /// Joins the last point back to the first
        bool loop = false;
        LineJoin join = LineJoin::Miter;
        /// The longest miter, in half widths, before a Miter corner is bevelled; also how far an inner corner may reach. At least 1
        float miterLimit = 4.0f;
        LineCap cap = LineCap::Butt;
    };

    /**
     * The mesh of a line through `points`, flat in the x, y plane (each vertex keeps its point's z), facing +Z, every triangle counter-
     * clockwise. One quad per segment and one triangle per bevelled corner; the width and colour blend along each segment. u runs from
     * 0 at the first point to 1 at the last (by length; back to 1 at the first point on a loop), v is 0 on the right side and 1 on the
     * left. A point closer than 1e-6 to the one before it (in x, y) is dropped, as is a loop's last point on its first. Empty, with
     * `error` saying why, for fewer than two distinct points or a point that is not finite. A width below 0 is 0. A loop of two points
     * is an open line.
     */
    [[nodiscard]] Renderer::Common::MeshData MakeLineMesh(std::span<const LinePoint> points, const LineShape &shape,
                                                          std::string *error = nullptr);

    /// The points of a line whose width and colour go from the start values at the first position to the end values at the last, by
    /// length along the line (a loop's closing segment counts). With no length at all, by index.
    [[nodiscard]] std::vector<LinePoint> LinePointsAlongLength(std::span<const Math::Vector3> positions, bool loop, float startWidth,
                                                               float endWidth, const Common::Color &startColor,
                                                               const Common::Color &endColor);

    /**
     * Triangles covering a simple polygon (concave allowed, no holes, not crossing itself), by ear clipping: indices into `points`, three
     * per triangle, every triangle counter-clockwise whichever way the points wind. Points in a line are allowed. Empty, with `error`
     * saying why, for under 3 or over kMaxTriangulatePoints points, a point that is not finite, no area, or a polygon that crosses
     * itself.
     */
    [[nodiscard]] std::vector<std::uint32_t> TriangulatePolygon2D(std::span<const Math::Vector2> points, std::string *error = nullptr);

    /// A mesh as a script describes it (Mesh.Create): positions, optionally one colour and one uv per position, and indices into the
    /// positions counted from `indexBase` (1 for Lua)
    struct ScriptMeshDesc
    {
        std::vector<Math::Vector3> positions;
        std::vector<Common::Color> colors;
        std::vector<Math::Vector2> uvs;
        std::vector<std::int64_t> indices;
        std::int64_t indexBase = 0;
    };

    /**
     * The mesh data for a script's description, or nullopt with `error` saying why: no positions or over kMaxScriptMeshVertices of
     * them, a position that is not finite, colours or uvs that are not one per position, no indices, over kMaxScriptMeshIndices of
     * them, a count that is not whole triangles, or an index outside the positions. The normal is +Z and the tangent +X on every
     * vertex; a missing colour is white and a missing uv (0, 0).
     */
    [[nodiscard]] std::optional<Renderer::Common::MeshData> MakeScriptMeshData(const ScriptMeshDesc &desc, std::string *error = nullptr);

    /**
     * What LineRenderer, TrailRenderer and MeshRenderer2D draw with: an unlit, double-sided Blend material (Alpha or Additive), the
     * texture if any, in the Transparent queue. The vertex colours times `tint` are what is drawn.
     *
     * The software renderer cannot blend: it draws the geometry opaque and leaves out every pixel whose final alpha is under half the
     * tint's alpha (Renderer::Common::BlendCutOutBelow), so a trail fading to transparent ends where it is half faded instead of drawing
     * a solid tail. Additive draws as Alpha there.
     */
    class Geometry2DDraw
    {
    public:
        Geometry2DDraw() = default;
        Geometry2DDraw(const Geometry2DDraw &) = delete;
        Geometry2DDraw &operator=(const Geometry2DDraw &) = delete;

        /// Draws `mesh` with `world`; nothing for a null mesh or renderer
        void Draw(Renderer::Common::IRenderer *renderer, const std::shared_ptr<const Mesh> &mesh, const Math::Matrix<float, 4, 4> &world,
                  Blend2D blend, const std::shared_ptr<Texture> &texture, const Common::Color &tint, RenderQueue queue,
                  const Renderer::Common::RenderState &state);
        void Bind(Renderer::Common::IRenderer *renderer) { _resources.Bind(renderer); }
        void Release(bool callRenderer) { _resources.Release(callRenderer); }
        [[nodiscard]] Renderer::Common::IRenderer *GetRenderer() const { return _resources.GetRenderer(); }
        /// The material of the last draw (null before the first), for tests
        [[nodiscard]] const std::shared_ptr<Material> &GetMaterial() const { return _material; }

    private:
        std::shared_ptr<Material> _material;
        std::vector<std::shared_ptr<Material>> _slots; // _material once per submesh
        MeshDrawing::DrawResources _resources;
    };

    /// Puts `data` into `mesh`: made on first use, replaced in place after (so the GPU copy is updated, not made again), null for
    /// empty data
    void UpdateGeometryMesh(std::shared_ptr<Mesh> &mesh, Renderer::Common::MeshData data);

    /**
     * The base of LineRenderer, TrailRenderer and MeshRenderer2D: the blend mode and sorting order they share (saved as "blendMode" and
     * "sortingOrder"), the Transparent queue, and their render resources. Like Sprite2D, lower sorting orders draw first.
     */
    class GeometryRenderer2D : public IRenderable
    {
    public:
        explicit GeometryRenderer2D(GameObject &gameObject);
        ~GeometryRenderer2D() override;

        [[nodiscard]] RenderQueueKey GetRenderQueue() const override;
        void Render(Renderer::Common::IRenderer *renderer) override;
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        void SetBlendMode(Blend2D blend) { _blendMode = blend; }
        [[nodiscard]] Blend2D GetBlendMode() const { return _blendMode; }
        /// Lower draws first, so further back
        void SetSortingOrder(int order) { _sortingOrder = order; }
        [[nodiscard]] int GetSortingOrder() const { return _sortingOrder; }

    protected:
        Blend2D _blendMode = Blend2D::Alpha;
        int _sortingOrder = 0;
        Geometry2DDraw _draw;
    };
}
