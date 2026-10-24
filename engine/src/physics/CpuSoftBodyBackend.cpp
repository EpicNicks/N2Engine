#include "engine/physics/CpuSoftBodyBackend.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/softbody/TetSkinning.hpp"

namespace N2Engine::Physics
{
    namespace
    {
        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        // The smallest tetrahedron volume that still gets a volume constraint and a share of the mass
        constexpr float kMinTetVolume = 1e-18f;
        constexpr float kMinLength = 1e-9f;
    }

    SoftBodyParams CpuSoftBodyBackend::Sanitized(SoftBodyParams params)
    {
        const SoftBodyParams defaults;
        params.density = std::clamp(Finite(params.density, defaults.density), 1e-3f, 1e7f);
        params.stiffness = std::clamp(Finite(params.stiffness, defaults.stiffness), 1.0f, 1e11f);
        params.poisson = std::clamp(Finite(params.poisson, defaults.poisson), 0.0f, 0.49f);
        params.damping = std::clamp(Finite(params.damping, defaults.damping), 0.0f, 1000.0f);
        params.friction = std::clamp(Finite(params.friction, defaults.friction), 0.0f, 1.0f);
        params.solverIterations = std::clamp<std::uint32_t>(params.solverIterations, 1u, kMaxIterations);
        params.gravity.x = Finite(params.gravity.x, 0.0f);
        params.gravity.y = Finite(params.gravity.y, 0.0f);
        params.gravity.z = Finite(params.gravity.z, 0.0f);
        params.groundHeight = Finite(params.groundHeight, 0.0f);
        return params;
    }

    CpuSoftBodyBackend::Body *CpuSoftBodyBackend::Find(const SoftBodyHandle handle)
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    const CpuSoftBodyBackend::Body *CpuSoftBodyBackend::Find(const SoftBodyHandle handle) const
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    void CpuSoftBodyBackend::ComputeMasses(Body &body)
    {
        std::vector<float> mass(body.position.size(), 0.0f);
        for (std::size_t t = 0; t < body.tets.size(); ++t)
        {
            const float share = body.params.density * body.restVolume[t] * 0.25f;
            for (const std::uint32_t node : body.tets[t])
            {
                mass[node] += share;
            }
        }
        body.invMass.resize(mass.size());
        for (std::size_t i = 0; i < mass.size(); ++i)
        {
            body.invMass[i] = mass[i] > 0.0f ? 1.0f / mass[i] : 0.0f;
        }
    }

    SoftBodyHandle CpuSoftBodyBackend::CreateBody(const SoftBodyDesc &desc)
    {
        if (!desc.tetMesh)
        {
            return kInvalidSoftBody;
        }
        const TetMeshData &mesh = *desc.tetMesh;
        if (mesh.positions.empty() || mesh.tets.empty() || mesh.tets.size() > kMaxTetsPerBody)
        {
            return kInvalidSoftBody;
        }
        for (const float element : desc.localToWorld)
        {
            if (!std::isfinite(element))
            {
                return kInvalidSoftBody;
            }
        }
        for (const auto &tet : mesh.tets)
        {
            for (const std::uint32_t node : tet)
            {
                if (node >= mesh.positions.size())
                {
                    return kInvalidSoftBody;
                }
            }
        }
        for (const TetEmbedding &embedding : mesh.surfaceEmbedding)
        {
            if (embedding.tet >= mesh.tets.size())
            {
                return kInvalidSoftBody;
            }
        }

        Body body;
        body.params = Sanitized(desc.params);
        body.mesh = desc.tetMesh;

        const float *m = desc.localToWorld;
        body.rest.resize(mesh.positions.size());
        for (std::size_t i = 0; i < mesh.positions.size(); ++i)
        {
            const Math::Vector3 &p = mesh.positions[i];
            body.rest[i] = {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
        }

        body.tets = mesh.tets;
        body.restVolume.resize(body.tets.size());
        body.flipped.assign(body.tets.size(), 0);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        pairs.reserve(body.tets.size() * 6);
        for (std::size_t t = 0; t < body.tets.size(); ++t)
        {
            auto &tet = body.tets[t];
            const V3 &a = body.rest[tet[0]];
            const V3 &b = body.rest[tet[1]];
            const V3 &c = body.rest[tet[2]];
            const V3 &d = body.rest[tet[3]];
            const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
            const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            const float e3x = d.x - a.x, e3y = d.y - a.y, e3z = d.z - a.z;
            float volume = ((e1y * e2z - e1z * e2y) * e3x + (e1z * e2x - e1x * e2z) * e3y +
                            (e1x * e2y - e1y * e2x) * e3z) / 6.0f;
            if (volume < 0.0f)
            {
                // A mirrored transform turned it inside out: swap two corners so every tetrahedron is positive
                // (the embedding's weights stay in the mesh's original corner order, so the tetrahedron is flagged
                // and CopySurfacePositions swaps the weights of corners 2 and 3 to match)
                std::swap(tet[2], tet[3]);
                body.flipped[t] = 1;
                volume = -volume;
            }
            body.restVolume[t] = volume >= kMinTetVolume ? volume : 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                for (int j = i + 1; j < 4; ++j)
                {
                    pairs.emplace_back(std::min(tet[i], tet[j]), std::max(tet[i], tet[j]));
                }
            }
        }
        std::ranges::sort(pairs);
        const auto duplicates = std::ranges::unique(pairs);
        pairs.erase(duplicates.begin(), duplicates.end());
        body.edges.reserve(pairs.size());
        for (const auto &[a, b] : pairs)
        {
            const V3 &pa = body.rest[a];
            const V3 &pb = body.rest[b];
            const float dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
            body.edges.push_back({a, b, std::sqrt(dx * dx + dy * dy + dz * dz)});
        }

        body.position = body.rest;
        body.previous = body.rest;
        body.velocity.assign(body.rest.size(), V3{});
        body.edgeLambda.assign(body.edges.size(), 0.0f);
        body.volumeLambda.assign(body.tets.size(), 0.0f);
        ComputeMasses(body);

        const std::uint32_t index = _nextIndex++;
        _bodies.emplace(index, std::move(body));
        return MakeSoftBodyHandle(GetGeneration(), index);
    }

    void CpuSoftBodyBackend::Destroy(const SoftBodyHandle handle)
    {
        if (OwnsHandle(handle))
        {
            _bodies.erase(SoftBodyHandleIndex(handle));
        }
    }

    void CpuSoftBodyBackend::SetParams(const SoftBodyHandle handle, const SoftBodyParams &params)
    {
        if (Body *body = Find(handle))
        {
            const float density = body->params.density;
            body->params = Sanitized(params);
            if (body->params.density != density)
            {
                ComputeMasses(*body);
            }
        }
    }

    void CpuSoftBodyBackend::AddVelocity(const SoftBodyHandle handle, const Math::Vector3 &velocity)
    {
        Body *body = Find(handle);
        if (!body || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
        {
            return;
        }
        for (std::size_t i = 0; i < body->velocity.size(); ++i)
        {
            if (body->invMass[i] > 0.0f)
            {
                body->velocity[i].x += velocity.x;
                body->velocity[i].y += velocity.y;
                body->velocity[i].z += velocity.z;
            }
        }
    }

    void CpuSoftBodyBackend::ResetToRest(Body &body)
    {
        body.position = body.rest;
        body.previous = body.rest;
        body.velocity.assign(body.rest.size(), V3{});
    }

    void CpuSoftBodyBackend::Step(const float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }
        for (auto &entry : _bodies)
        {
            Simulate(entry.second, dt);
        }
    }

    void CpuSoftBodyBackend::Simulate(Body &body, const float dt)
    {
        const SoftBodyParams &params = body.params;
        const float h = dt / static_cast<float>(kSubSteps);
        const float h2 = h * h;

        // Young's modulus and Poisson's ratio to the two compliances (see the class comment)
        const float shear = params.stiffness / (2.0f * (1.0f + params.poisson));
        const float bulk = params.stiffness / (3.0f * (1.0f - 2.0f * params.poisson));
        const float edgeAlphaBase = 1.0f / (shear * h2);  // divided by the edge's rest length below
        const float volumeAlphaBase = 1.0f / (bulk * h2); // times the tetrahedron's rest volume below
        const float damping = 1.0f / (1.0f + params.damping * h);

        const std::size_t nodeCount = body.position.size();
        for (int sub = 0; sub < kSubSteps; ++sub)
        {
            // Predict
            for (std::size_t i = 0; i < nodeCount; ++i)
            {
                body.previous[i] = body.position[i];
                if (body.invMass[i] <= 0.0f)
                {
                    continue;
                }
                V3 &v = body.velocity[i];
                v.x = (v.x + params.gravity.x * h) * damping;
                v.y = (v.y + params.gravity.y * h) * damping;
                v.z = (v.z + params.gravity.z * h) * damping;
                body.position[i].x += v.x * h;
                body.position[i].y += v.y * h;
                body.position[i].z += v.z * h;
            }

            std::ranges::fill(body.edgeLambda, 0.0f);
            std::ranges::fill(body.volumeLambda, 0.0f);

            for (std::uint32_t iteration = 0; iteration < params.solverIterations; ++iteration)
            {
                // Distance constraints
                for (std::size_t e = 0; e < body.edges.size(); ++e)
                {
                    const Edge &edge = body.edges[e];
                    const float wa = body.invMass[edge.a];
                    const float wb = body.invMass[edge.b];
                    const float wSum = wa + wb;
                    if (wSum <= 0.0f || edge.restLength <= kMinLength)
                    {
                        continue;
                    }
                    V3 &pa = body.position[edge.a];
                    V3 &pb = body.position[edge.b];
                    const float dx = pa.x - pb.x, dy = pa.y - pb.y, dz = pa.z - pb.z;
                    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (length < kMinLength)
                    {
                        continue;
                    }
                    const float alpha = edgeAlphaBase / edge.restLength;
                    const float c = length - edge.restLength;
                    const float delta = (-c - alpha * body.edgeLambda[e]) / (wSum + alpha);
                    body.edgeLambda[e] += delta;
                    const float scale = delta / length;
                    pa.x += wa * scale * dx;
                    pa.y += wa * scale * dy;
                    pa.z += wa * scale * dz;
                    pb.x -= wb * scale * dx;
                    pb.y -= wb * scale * dy;
                    pb.z -= wb * scale * dz;
                }

                // Volume constraints
                for (std::size_t t = 0; t < body.tets.size(); ++t)
                {
                    const float restVolume = body.restVolume[t];
                    if (restVolume <= 0.0f)
                    {
                        continue;
                    }
                    const auto &tet = body.tets[t];
                    V3 &p0 = body.position[tet[0]];
                    V3 &p1 = body.position[tet[1]];
                    V3 &p2 = body.position[tet[2]];
                    V3 &p3 = body.position[tet[3]];
                    const float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z;
                    const float e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;
                    const float e3x = p3.x - p0.x, e3y = p3.y - p0.y, e3z = p3.z - p0.z;
                    // The gradients of the volume with respect to nodes 1, 2 and 3 (node 0's is minus their sum)
                    const float g1x = (e2y * e3z - e2z * e3y) / 6.0f;
                    const float g1y = (e2z * e3x - e2x * e3z) / 6.0f;
                    const float g1z = (e2x * e3y - e2y * e3x) / 6.0f;
                    const float g2x = (e3y * e1z - e3z * e1y) / 6.0f;
                    const float g2y = (e3z * e1x - e3x * e1z) / 6.0f;
                    const float g2z = (e3x * e1y - e3y * e1x) / 6.0f;
                    const float g3x = (e1y * e2z - e1z * e2y) / 6.0f;
                    const float g3y = (e1z * e2x - e1x * e2z) / 6.0f;
                    const float g3z = (e1x * e2y - e1y * e2x) / 6.0f;
                    const float g0x = -(g1x + g2x + g3x);
                    const float g0y = -(g1y + g2y + g3y);
                    const float g0z = -(g1z + g2z + g3z);
                    const float volume = (g1x * e1x + g1y * e1y + g1z * e1z);

                    const float w0 = body.invMass[tet[0]];
                    const float w1 = body.invMass[tet[1]];
                    const float w2 = body.invMass[tet[2]];
                    const float w3 = body.invMass[tet[3]];
                    const float alpha = volumeAlphaBase * restVolume;
                    const float denominator = w0 * (g0x * g0x + g0y * g0y + g0z * g0z) +
                                              w1 * (g1x * g1x + g1y * g1y + g1z * g1z) +
                                              w2 * (g2x * g2x + g2y * g2y + g2z * g2z) +
                                              w3 * (g3x * g3x + g3y * g3y + g3z * g3z) + alpha;
                    if (!(denominator > 0.0f))
                    {
                        continue;
                    }
                    const float c = volume - restVolume;
                    const float delta = (-c - alpha * body.volumeLambda[t]) / denominator;
                    body.volumeLambda[t] += delta;
                    p0.x += w0 * delta * g0x;
                    p0.y += w0 * delta * g0y;
                    p0.z += w0 * delta * g0z;
                    p1.x += w1 * delta * g1x;
                    p1.y += w1 * delta * g1y;
                    p1.z += w1 * delta * g1z;
                    p2.x += w2 * delta * g2x;
                    p2.y += w2 * delta * g2y;
                    p2.z += w2 * delta * g2z;
                    p3.x += w3 * delta * g3x;
                    p3.y += w3 * delta * g3y;
                    p3.z += w3 * delta * g3z;
                }
            }

            // The ground plane: clamp, and take friction * depth off the tangential motion of this sub-step
            if (params.groundEnabled)
            {
                for (std::size_t i = 0; i < nodeCount; ++i)
                {
                    V3 &p = body.position[i];
                    if (!(p.y < params.groundHeight))
                    {
                        continue;
                    }
                    const float depth = params.groundHeight - p.y;
                    p.y = params.groundHeight;
                    const float tx = p.x - body.previous[i].x;
                    const float tz = p.z - body.previous[i].z;
                    const float length = std::sqrt(tx * tx + tz * tz);
                    if (length > kMinLength)
                    {
                        const float keep = std::max(0.0f, length - params.friction * depth) / length;
                        p.x = body.previous[i].x + tx * keep;
                        p.z = body.previous[i].z + tz * keep;
                    }
                }
            }

            // Velocities from the motion
            const float inverseH = 1.0f / h;
            for (std::size_t i = 0; i < nodeCount; ++i)
            {
                body.velocity[i].x = (body.position[i].x - body.previous[i].x) * inverseH;
                body.velocity[i].y = (body.position[i].y - body.previous[i].y) * inverseH;
                body.velocity[i].z = (body.position[i].z - body.previous[i].z) * inverseH;
            }
        }

        // A body that blew up (a non-finite position) restarts from its rest shape rather than poisoning the picture
        float check = 0.0f;
        for (const V3 &p : body.position)
        {
            check += p.x + p.y + p.z;
        }
        if (!std::isfinite(check))
        {
            ResetToRest(body);
        }
    }

    void CpuSoftBodyBackend::CopySurfacePositions(const SoftBodyHandle handle, std::vector<Math::Vector3> &out) const
    {
        out.clear();
        const Body *body = Find(handle);
        if (!body)
        {
            return;
        }
        // The embedding and skinning code both soft-body backends share
        SoftBodies::SkinSurface<V3>(*body->mesh, body->tets, body->flipped, body->position, out);
    }

    void CpuSoftBodyBackend::CopyNodePositions(const SoftBodyHandle handle, std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (const Body *body = Find(handle))
        {
            for (const V3 &p : body->position)
            {
                out.emplace_back(p.x, p.y, p.z);
            }
        }
    }

    void CpuSoftBodyBackend::CopyNodeVelocities(const SoftBodyHandle handle, std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (const Body *body = Find(handle))
        {
            for (const V3 &v : body->velocity)
            {
                out.emplace_back(v.x, v.y, v.z);
            }
        }
    }

    float CpuSoftBodyBackend::GetVolume(const SoftBodyHandle handle) const
    {
        const Body *body = Find(handle);
        if (!body)
        {
            return 0.0f;
        }
        double total = 0.0;
        for (const auto &tet : body->tets)
        {
            const V3 &a = body->position[tet[0]];
            const V3 &b = body->position[tet[1]];
            const V3 &c = body->position[tet[2]];
            const V3 &d = body->position[tet[3]];
            const double e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
            const double e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            const double e3x = d.x - a.x, e3y = d.y - a.y, e3z = d.z - a.z;
            total += ((e1y * e2z - e1z * e2y) * e3x + (e1z * e2x - e1x * e2z) * e3y + (e1x * e2y - e1y * e2x) * e3z) /
                     6.0;
        }
        return static_cast<float>(total);
    }

    float CpuSoftBodyBackend::GetRestVolume(const SoftBodyHandle handle) const
    {
        const Body *body = Find(handle);
        if (!body)
        {
            return 0.0f;
        }
        double total = 0.0;
        for (const float volume : body->restVolume)
        {
            total += volume;
        }
        return static_cast<float>(total);
    }

    std::size_t CpuSoftBodyBackend::GetTetCount(const SoftBodyHandle handle) const
    {
        const Body *body = Find(handle);
        return body ? body->tets.size() : 0;
    }
}
