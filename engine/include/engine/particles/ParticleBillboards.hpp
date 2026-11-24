#pragma once

#include <span>

#include <math/Vector4.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/RenderCamera.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Particles
{
    /// Which way a particle's quad faces (issue #445)
    enum class ParticleRenderMode
    {
        /// Faces the camera (the default)
        Billboard,
        /// Faces the camera, stretched along the particle's velocity as seen by the camera: as wide as the particle's
        /// size, as long as size * lengthScale + speed * speedScale. A particle moving straight at the camera (or not at
        /// all) is drawn as a Billboard. Not rotated.
        Stretched,
        /// Lies flat on the world XZ plane, facing up (+Y): ripples and decals on the ground
        HorizontalBillboard,
        /// Stands upright (its up is world +Y) and turns about Y to face the camera
        VerticalBillboard
    };

    N2_SERIALIZE_ENUM(ParticleRenderMode, {
                      { ParticleRenderMode::Billboard, "Billboard" },
                      { ParticleRenderMode::Stretched, "Stretched" },
                      { ParticleRenderMode::HorizontalBillboard, "HorizontalBillboard" },
                      { ParticleRenderMode::VerticalBillboard, "VerticalBillboard" }
                      })

    /// How a particle looks over its life: its size (the quad's edge, in world units) and colour go linearly from
    /// the start to the end value as its age goes from 0 to 1
    struct BillboardStyle
    {
        float startSize = 0.2f;
        float endSize = 0.2f;
        Common::Color startColor{Common::Color::White};
        Common::Color endColor{Common::Color::White};
        ParticleRenderMode mode = ParticleRenderMode::Billboard;
        /// Stretched: the quad's length is size * lengthScale + speed * speedScale
        float lengthScale = 2.0f;
        float speedScale = 0.0f;
    };

    /**
     * Replaces `mesh` with one quad per particle (4 vertices, 6 indices, in the order of `particles`: x, y, z the world
     * position, w the age fraction, as IParticleBackend::CopyPositions gives them), facing as style.mode says. The
     * corners are bottom-left, bottom-right, top-right, top-left of the quad's own right and up axes (counter-clockwise
     * seen from the side it faces), uvs (0,0) (1,0) (1,1) (0,1), the normal the side it faces, and the vertex colour the
     * particle's colour at its age. A Stretched quad's up axis is the particle's direction of travel (v = 1 at its
     * front). The quads are in world space: draw them with an identity model matrix.
     *
     * `motion`, when it has one entry per particle (IParticleBackend::CopyMotion: x, y, z the velocity, w the rotation in
     * radians), turns each quad by its rotation (counter-clockwise seen from the side it faces; not Stretched) and gives
     * Stretched its direction and speed. Without it the quads are unrotated and Stretched draws Billboards.
     *
     * The vectors keep their capacity from call to call, and the indices (which depend only on the count) are
     * written only for quads the mesh did not have before, so a frame with the same count or fewer rebuilds only
     * the vertices.
     */
    void BuildBillboards(Renderer::Common::MeshData &mesh, std::span<const Math::Vector4> particles,
                         const Rendering::CameraBasis &camera, const BillboardStyle &style,
                         std::span<const Math::Vector4> motion = {});
}
