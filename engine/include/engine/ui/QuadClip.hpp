#pragma once

#include <renderer/common/RenderTypes.hpp>

#include "engine/ui/Transform2D.hpp"

namespace N2Engine::UI
{
    /**
     * `quads` cut to a convex counter-clockwise polygon in the mesh's own x/y space (issue #445: a RectMask turned
     * against the graphic it clips, which no rect can describe). Every four consecutive vertices are one convex quad,
     * as UI images and text build them; a quad wholly inside keeps its vertices, one across an edge becomes the polygon
     * left of it, with positions, texture coordinates and colours interpolated along the cut edges, and one outside is
     * dropped. The result is a triangle fan per quad, so it is not a list of quads any more. Vertices left over after
     * the last whole quad are ignored.
     */
    [[nodiscard]] Renderer::Common::MeshData ClipQuadMesh(const Renderer::Common::MeshData &quads, const ConvexPolygon &clip);
}
