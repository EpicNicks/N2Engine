#include "engine/particles/ParticleBillboards.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace N2Engine::Particles
{
    namespace
    {
        float Lerp(const float a, const float b, const float t)
        {
            return a + (b - a) * t;
        }

        /// Plain float maths, so the builder needs nothing of the vector types beyond their fields
        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        V3 Of(const Math::Vector3 &v)
        {
            return {v.x, v.y, v.z};
        }

        V3 Cross(const V3 &a, const V3 &b)
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }

        float Dot(const V3 &a, const V3 &b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        V3 Scaled(const V3 &v, const float s)
        {
            return {v.x * s, v.y * s, v.z * s};
        }

        V3 Plus(const V3 &a, const V3 &b)
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }

        /// The quad's axes: the corners are centre + right * x + up * y; normal is the side it faces
        struct Frame
        {
            V3 right;
            V3 up;
            V3 normal;
        };

        /// `frame` turned by `angle` radians in its own plane, counter-clockwise seen from its normal
        Frame Rotated(const Frame &frame, const float angle)
        {
            if (angle == 0.0f || !std::isfinite(angle))
            {
                return frame;
            }
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return {Plus(Scaled(frame.right, c), Scaled(frame.up, s)), Plus(Scaled(frame.right, -s), Scaled(frame.up, c)),
                    frame.normal};
        }
    }

    void BuildBillboards(Renderer::Common::MeshData &mesh, const std::span<const Math::Vector4> particles,
                         const Rendering::CameraBasis &camera, const BillboardStyle &style,
                         const std::span<const Math::Vector4> motion)
    {
        using Renderer::Common::Vertex;

        const std::size_t count = particles.size();
        mesh.vertices.resize(count * 4);

        const std::size_t oldQuads = mesh.indices.size() / 6;
        mesh.indices.resize(count * 6);
        for (std::size_t q = oldQuads; q < count; ++q)
        {
            const auto base = static_cast<std::uint32_t>(q * 4);
            std::uint32_t *i = &mesh.indices[q * 6];
            i[0] = base;
            i[1] = base + 1;
            i[2] = base + 2;
            i[3] = base;
            i[4] = base + 2;
            i[5] = base + 3;
        }

        // The corners as multiples of (right, up), in the order bottom-left, bottom-right, top-right, top-left
        static constexpr float kCornerX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        static constexpr float kCornerY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static constexpr float kV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

        const V3 forward = Of(camera.forward);
        const V3 towardCamera = Scaled(forward, -1.0f);
        const bool hasMotion = motion.size() == count;

        // The frame every particle of the mode shares before its own rotation
        Frame shared{Of(camera.right), Of(camera.up), towardCamera};
        if (style.mode == ParticleRenderMode::HorizontalBillboard)
        {
            // Flat on XZ, facing +Y: right x up = (1, 0, 0) x (0, 0, -1) = (0, 1, 0)
            shared = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}};
        }
        else if (style.mode == ParticleRenderMode::VerticalBillboard)
        {
            // Upright, turned about Y towards the camera: the camera's right without its vertical part
            V3 right{camera.right.x, 0.0f, camera.right.z};
            const float length = std::sqrt(Dot(right, right));
            right = length > 1e-6f ? Scaled(right, 1.0f / length) : V3{1.0f, 0.0f, 0.0f};
            const V3 up{0.0f, 1.0f, 0.0f};
            shared = {right, up, Cross(right, up)};
        }

        for (std::size_t p = 0; p < count; ++p)
        {
            const Math::Vector4 &particle = particles[p];
            const float age = std::clamp(particle.w, 0.0f, 1.0f);
            const float size = std::max(0.0f, Lerp(style.startSize, style.endSize, age));
            float halfWidth = 0.5f * size;
            float halfLength = halfWidth;
            const float r = Lerp(style.startColor.r, style.endColor.r, age);
            const float g = Lerp(style.startColor.g, style.endColor.g, age);
            const float b = Lerp(style.startColor.b, style.endColor.b, age);
            const float a = Lerp(style.startColor.a, style.endColor.a, age);

            Frame frame = shared;
            if (hasMotion)
            {
                const Math::Vector4 &m = motion[p];
                if (style.mode == ParticleRenderMode::Stretched)
                {
                    // The velocity as the camera sees it: without its part along the view direction
                    const V3 velocity{m.x, m.y, m.z};
                    const float speed = std::sqrt(Dot(velocity, velocity));
                    const V3 across = Plus(velocity, Scaled(forward, -Dot(velocity, forward)));
                    const float acrossLength = std::sqrt(Dot(across, across));
                    if (std::isfinite(speed) && acrossLength > 1e-6f && std::isfinite(acrossLength))
                    {
                        // up along the travel, right = forward x up, so right x up faces the camera
                        const V3 up = Scaled(across, 1.0f / acrossLength);
                        frame = {Cross(forward, up), up, towardCamera};
                        halfLength = 0.5f * std::max(0.0f, size * style.lengthScale + speed * style.speedScale);
                        if (!std::isfinite(halfLength))
                        {
                            halfLength = halfWidth;
                        }
                    }
                }
                else
                {
                    frame = Rotated(shared, m.w);
                }
            }

            for (int c = 0; c < 4; ++c)
            {
                Vertex &vertex = mesh.vertices[p * 4 + static_cast<std::size_t>(c)];
                const float sx = kCornerX[c] * halfWidth;
                const float sy = kCornerY[c] * halfLength;
                vertex.position[0] = particle.x + frame.right.x * sx + frame.up.x * sy;
                vertex.position[1] = particle.y + frame.right.y * sx + frame.up.y * sy;
                vertex.position[2] = particle.z + frame.right.z * sx + frame.up.z * sy;
                vertex.normal[0] = frame.normal.x;
                vertex.normal[1] = frame.normal.y;
                vertex.normal[2] = frame.normal.z;
                vertex.texCoord[0] = kU[c];
                vertex.texCoord[1] = kV[c];
                vertex.color[0] = r;
                vertex.color[1] = g;
                vertex.color[2] = b;
                vertex.color[3] = a;
                vertex.tangent[0] = 0.0f;
                vertex.tangent[1] = 0.0f;
                vertex.tangent[2] = 0.0f;
                vertex.tangent[3] = 0.0f;
            }
        }
    }
}
