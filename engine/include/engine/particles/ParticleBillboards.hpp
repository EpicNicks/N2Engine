#pragma once

#include <span>

#include <math/Vector4.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/RenderCamera.hpp"

namespace N2Engine::Particles
{
    /// How a particle looks over its life: its size (the quad's edge, in world units) and colour go linearly from
    /// the start to the end value as its age goes from 0 to 1
    struct BillboardStyle
    {
        float startSize = 0.2f;
        float endSize = 0.2f;
        Common::Color startColor{Common::Color::White};
        Common::Color endColor{Common::Color::White};
    };

    /**
     * Replaces `mesh` with one camera-facing quad per particle (4 vertices, 6 indices, in the order of
     * `particles`: x, y, z the world position, w the age fraction, as IParticleBackend::CopyPositions gives them).
     * The corners are bottom-left, bottom-right, top-right, top-left of the camera's view (counter-clockwise seen
     * from the camera), uvs (0,0) (1,0) (1,1) (0,1), the normal pointing back at the camera, and the vertex colour
     * the particle's colour at its age. The quads are in world space: draw them with an identity model matrix.
     *
     * The vectors keep their capacity from call to call, and the indices (which depend only on the count) are
     * written only for quads the mesh did not have before, so a frame with the same count or fewer rebuilds only
     * the vertices.
     */
    void BuildBillboards(Renderer::Common::MeshData &mesh, std::span<const Math::Vector4> particles,
                         const Rendering::CameraBasis &camera, const BillboardStyle &style);
}
