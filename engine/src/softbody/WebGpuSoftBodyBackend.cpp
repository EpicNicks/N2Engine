#include "engine/softbody/WebGpuSoftBodyBackend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

#include <renderer/webgpu/WgslSources.hpp>

#include "engine/softbody/TetSkinning.hpp"

namespace N2Engine::SoftBodies
{
    using Renderer::Common::GpuBindingKind;
    using Renderer::Common::GpuBufferUsage;
    using Renderer::Common::GpuReadStatus;

    /// The WGSL Params block of softbody.wgsl, member for member (96 bytes; each vec3 first in its 16 bytes)
    struct WebGpuSoftBodyBackend::GpuParams
    {
        float gravity[3];
        float h;
        float push[3];
        float keep;
        std::uint32_t nodeCount;
        std::uint32_t edgeCount;
        std::uint32_t tetCount;
        std::uint32_t subSteps;
        std::uint32_t iterations;
        std::uint32_t edgeColours;
        std::uint32_t tetColours;
        std::uint32_t groundEnabled;
        float edgeAlphaBase;
        float volumeAlphaBase;
        float groundHeight;
        float friction;
        float invMassScale;
        float pad[3];
    };

    namespace
    {
        // As the CPU backend (CpuSoftBodyBackend.cpp)
        constexpr float kMinTetVolume = 1e-18f;

        // group 0: Params, then pos, prev, vel (read-write), edges, tets, ranges (read-only), lambdas (read-write)
        constexpr std::array<GpuBindingKind, 8> kBindings = {
            GpuBindingKind::Uniform,         GpuBindingKind::Storage,         GpuBindingKind::Storage,
            GpuBindingKind::Storage,         GpuBindingKind::ReadOnlyStorage, GpuBindingKind::ReadOnlyStorage,
            GpuBindingKind::ReadOnlyStorage, GpuBindingKind::Storage};

        struct GpuEdge
        {
            std::uint32_t a;
            std::uint32_t b;
            float rest;
            std::uint32_t pad;
        };
        static_assert(sizeof(GpuEdge) == 16, "must match Edge in softbody.wgsl");

        struct GpuTet
        {
            std::uint32_t nodes[4];
            float rest;
            float pad[3];
        };
        static_assert(sizeof(GpuTet) == 32, "must match Tet in softbody.wgsl");

        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        /// The CPU backend's sanitising (CpuSoftBodyBackend::Sanitized), so both read a SoftBody's settings the same way
        Physics::SoftBodyParams Sanitized(Physics::SoftBodyParams params)
        {
            const Physics::SoftBodyParams defaults;
            params.density = std::clamp(Finite(params.density, defaults.density), 1e-3f, 1e7f);
            params.stiffness = std::clamp(Finite(params.stiffness, defaults.stiffness), 1.0f, 1e11f);
            params.poisson = std::clamp(Finite(params.poisson, defaults.poisson), 0.0f, 0.49f);
            params.damping = std::clamp(Finite(params.damping, defaults.damping), 0.0f, 1000.0f);
            params.friction = std::clamp(Finite(params.friction, defaults.friction), 0.0f, 1.0f);
            params.solverIterations =
                std::clamp<std::uint32_t>(params.solverIterations, 1u, WebGpuSoftBodyBackend::kMaxIterations);
            params.gravity.x = Finite(params.gravity.x, 0.0f);
            params.gravity.y = Finite(params.gravity.y, 0.0f);
            params.gravity.z = Finite(params.gravity.z, 0.0f);
            params.groundHeight = Finite(params.groundHeight, 0.0f);
            return params;
        }

        /**
         * Greedy colouring: constraint i gets the lowest colour none of its nodes has had yet, so no two constraints of
         * one colour share a node and the GPU may solve a colour's constraints at the same time. `nodesOf(i)` gives the
         * nodes of constraint i. Returns the colour of every constraint; `colourCount` gets the number of colours.
         */
        template <typename NodesOf>
        std::vector<std::uint32_t> Colour(const std::size_t constraints, const std::size_t nodeCount, NodesOf nodesOf,
                                          std::uint32_t &colourCount)
        {
            std::vector<std::vector<std::uint64_t>> used(nodeCount); // per node: a bit per colour it already has
            std::vector<std::uint32_t> colours(constraints, 0);
            colourCount = 0;
            for (std::size_t c = 0; c < constraints; ++c)
            {
                const auto nodes = nodesOf(c);
                std::uint32_t colour = 0;
                for (;; ++colour)
                {
                    const std::size_t word = colour / 64;
                    const std::uint64_t bit = std::uint64_t{1} << (colour % 64);
                    bool taken = false;
                    for (const std::uint32_t node : nodes)
                    {
                        if (word < used[node].size() && (used[node][word] & bit) != 0)
                        {
                            taken = true;
                            break;
                        }
                    }
                    if (!taken)
                    {
                        break;
                    }
                }
                const std::size_t word = colour / 64;
                for (const std::uint32_t node : nodes)
                {
                    if (used[node].size() <= word)
                    {
                        used[node].resize(word + 1, 0);
                    }
                    used[node][word] |= std::uint64_t{1} << (colour % 64);
                }
                colours[c] = colour;
                colourCount = std::max(colourCount, colour + 1);
            }
            return colours;
        }

        /// The constraint order that groups `colours` (stable within a colour), and [first, end) of every colour in it
        std::vector<std::uint32_t> SortByColour(const std::vector<std::uint32_t> &colours, const std::uint32_t colourCount,
                                                std::vector<std::uint32_t> &ranges)
        {
            std::vector<std::uint32_t> starts(colourCount + 1, 0);
            for (const std::uint32_t colour : colours)
            {
                ++starts[colour + 1];
            }
            for (std::uint32_t c = 0; c < colourCount; ++c)
            {
                starts[c + 1] += starts[c];
            }
            for (std::uint32_t c = 0; c < colourCount; ++c)
            {
                ranges.push_back(starts[c]);
                ranges.push_back(starts[c + 1]);
            }
            std::vector<std::uint32_t> order(colours.size());
            std::vector<std::uint32_t> next(starts.begin(), starts.end() - 1);
            for (std::uint32_t i = 0; i < colours.size(); ++i)
            {
                order[next[colours[i]]++] = i;
            }
            return order;
        }

        template <typename T>
        std::uint64_t BytesOf(const std::vector<T> &values)
        {
            // A WebGPU buffer can't be empty: an empty list still gets one 16-byte element (never read)
            return std::max<std::uint64_t>(values.size() * sizeof(T), 16);
        }
    }

    WebGpuSoftBodyBackend::WebGpuSoftBodyBackend(Renderer::Common::IGpuCompute *compute, std::weak_ptr<const void> lifetime)
        : _compute(compute), _lifetime(std::move(lifetime))
    {
        if (Usable())
        {
            _simulate = _compute->CreateKernel("N2Engine soft bodies", Renderer::WebGpu::Wgsl::kSoftBodyComputeWgsl,
                                               "simulate", kBindings);
        }
        if (_simulate == 0)
        {
            _broken = true;
        }
    }

    WebGpuSoftBodyBackend::~WebGpuSoftBodyBackend()
    {
        for (auto &entry : _bodies)
        {
            ReleaseBuffers(entry.second);
        }
    }

    bool WebGpuSoftBodyBackend::Usable() const
    {
        return !_broken && _compute != nullptr && !_lifetime.expired() && _compute->IsUsable();
    }

    bool WebGpuSoftBodyBackend::IsHealthy() const
    {
        return Usable();
    }

    WebGpuSoftBodyBackend::Body *WebGpuSoftBodyBackend::Find(const Physics::SoftBodyHandle handle)
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(Physics::SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    const WebGpuSoftBodyBackend::Body *WebGpuSoftBodyBackend::Find(const Physics::SoftBodyHandle handle) const
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(Physics::SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    void WebGpuSoftBodyBackend::ReleaseBuffers(Body &body)
    {
        // The compute belongs to the renderer: once its lifetime token expired, its buffers are gone with it
        if (_compute && !_lifetime.expired())
        {
            if (body.ticket != 0)
            {
                _compute->CancelRead(body.ticket);
            }
            for (const Renderer::Common::GpuBufferId buffer : {body.uniform, body.positions, body.previous,
                                                               body.velocities, body.edges, body.tetBuffer, body.ranges,
                                                               body.lambdas})
            {
                if (buffer != 0)
                {
                    _compute->DestroyBuffer(buffer);
                }
            }
        }
        body.ticket = 0;
        body.uniform = body.positions = body.previous = body.velocities = 0;
        body.edges = body.tetBuffer = body.ranges = body.lambdas = 0;
    }

    Physics::SoftBodyHandle WebGpuSoftBodyBackend::CreateBody(const Physics::SoftBodyDesc &desc)
    {
        if (!Usable() || !desc.tetMesh)
        {
            return Physics::kInvalidSoftBody;
        }
        // What the CPU backend refuses, refused the same way
        const Physics::TetMeshData &mesh = *desc.tetMesh;
        if (mesh.positions.empty() || mesh.tets.empty() || mesh.tets.size() > Physics::kMaxTetsPerBody)
        {
            return Physics::kInvalidSoftBody;
        }
        for (const float element : desc.localToWorld)
        {
            if (!std::isfinite(element))
            {
                return Physics::kInvalidSoftBody;
            }
        }
        for (const auto &tet : mesh.tets)
        {
            for (const std::uint32_t node : tet)
            {
                if (node >= mesh.positions.size())
                {
                    return Physics::kInvalidSoftBody;
                }
            }
        }
        for (const Physics::TetEmbedding &embedding : mesh.surfaceEmbedding)
        {
            if (embedding.tet >= mesh.tets.size())
            {
                return Physics::kInvalidSoftBody;
            }
        }

        Body body;
        body.params = Sanitized(desc.params);
        body.mesh = desc.tetMesh;
        body.massDensity = body.params.density;

        // The rest shape in world space with every tetrahedron positively oriented, the rest volumes, the unique edges
        // with their rest lengths and the masses: the CPU backend's set-up
        std::vector<Math::Vector3> rest;
        OrientTets(mesh, desc.localToWorld, rest, body.tets, body.flipped);
        body.nodeCount = static_cast<std::uint32_t>(rest.size());
        body.tetCount = static_cast<std::uint32_t>(body.tets.size());

        std::vector<float> restVolume(body.tets.size(), 0.0f);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        pairs.reserve(body.tets.size() * 6);
        for (std::size_t t = 0; t < body.tets.size(); ++t)
        {
            const auto &tet = body.tets[t];
            const float volume = TetVolume6(rest[tet[0]], rest[tet[1]], rest[tet[2]], rest[tet[3]]) / 6.0f;
            restVolume[t] = volume >= kMinTetVolume ? volume : 0.0f;
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
        body.edgeCount = static_cast<std::uint32_t>(pairs.size());

        std::vector<float> mass(rest.size(), 0.0f);
        for (std::size_t t = 0; t < body.tets.size(); ++t)
        {
            const float share = body.params.density * restVolume[t] * 0.25f;
            for (const std::uint32_t node : body.tets[t])
            {
                mass[node] += share;
            }
        }
        body.restNodes.resize(rest.size() * 4);
        body.nodes = rest;
        for (std::size_t i = 0; i < rest.size(); ++i)
        {
            body.restNodes[i * 4 + 0] = rest[i].x;
            body.restNodes[i * 4 + 1] = rest[i].y;
            body.restNodes[i * 4 + 2] = rest[i].z;
            body.restNodes[i * 4 + 3] = mass[i] > 0.0f ? 1.0f / mass[i] : 0.0f;
        }

        // Colour the constraints and lay them out colour by colour
        std::vector<std::uint32_t> ranges;
        const std::vector<std::uint32_t> edgeColour = Colour(
            pairs.size(), rest.size(),
            [&pairs](const std::size_t e) { return std::array<std::uint32_t, 2>{pairs[e].first, pairs[e].second}; },
            body.edgeColours);
        const std::vector<std::uint32_t> edgeOrder = SortByColour(edgeColour, body.edgeColours, ranges);
        const std::vector<std::uint32_t> tetColour =
            Colour(body.tets.size(), rest.size(), [&body](const std::size_t t) { return body.tets[t]; }, body.tetColours);
        std::vector<std::uint32_t> tetRanges;
        const std::vector<std::uint32_t> tetOrder = SortByColour(tetColour, body.tetColours, tetRanges);
        ranges.insert(ranges.end(), tetRanges.begin(), tetRanges.end());

        std::vector<GpuEdge> edges(pairs.size());
        for (std::size_t i = 0; i < edgeOrder.size(); ++i)
        {
            const auto &[a, b] = pairs[edgeOrder[i]];
            const float dx = rest[a].x - rest[b].x, dy = rest[a].y - rest[b].y, dz = rest[a].z - rest[b].z;
            edges[i] = GpuEdge{a, b, std::sqrt(dx * dx + dy * dy + dz * dz), 0};
        }
        std::vector<GpuTet> tets(body.tets.size());
        for (std::size_t i = 0; i < tetOrder.size(); ++i)
        {
            const auto &tet = body.tets[tetOrder[i]];
            tets[i] = GpuTet{{tet[0], tet[1], tet[2], tet[3]}, restVolume[tetOrder[i]], {0.0f, 0.0f, 0.0f}};
        }

        const std::uint64_t nodeBytes = std::uint64_t{body.nodeCount} * 16;
        const std::uint64_t lambdaBytes = std::max<std::uint64_t>((std::uint64_t{body.edgeCount} + body.tetCount) * 4, 16);
        body.uniform = _compute->CreateBuffer(GpuBufferUsage::Uniform, sizeof(GpuParams));
        body.positions = _compute->CreateBuffer(GpuBufferUsage::Storage, nodeBytes);
        body.previous = _compute->CreateBuffer(GpuBufferUsage::Storage, nodeBytes);
        body.velocities = _compute->CreateBuffer(GpuBufferUsage::Storage, nodeBytes);
        body.edges = _compute->CreateBuffer(GpuBufferUsage::Storage, BytesOf(edges));
        body.tetBuffer = _compute->CreateBuffer(GpuBufferUsage::Storage, BytesOf(tets));
        body.ranges = _compute->CreateBuffer(GpuBufferUsage::Storage, BytesOf(ranges));
        body.lambdas = _compute->CreateBuffer(GpuBufferUsage::Storage, lambdaBytes);
        bool ok = body.uniform && body.positions && body.previous && body.velocities && body.edges && body.tetBuffer &&
                  body.ranges && body.lambdas;
        ok = ok && _compute->WriteBuffer(body.positions, 0, body.restNodes.data(), nodeBytes);
        ok = ok && (edges.empty() || _compute->WriteBuffer(body.edges, 0, edges.data(), edges.size() * sizeof(GpuEdge)));
        ok = ok && _compute->WriteBuffer(body.tetBuffer, 0, tets.data(), tets.size() * sizeof(GpuTet));
        ok = ok && (ranges.empty() ||
                    _compute->WriteBuffer(body.ranges, 0, ranges.data(), ranges.size() * sizeof(std::uint32_t)));
        if (!ok)
        {
            // Too big for the device, or out of memory: this body goes to the CPU, the backend stays usable
            ReleaseBuffers(body);
            return Physics::kInvalidSoftBody;
        }

        const std::uint32_t index = _nextIndex++;
        _bodies.emplace(index, std::move(body));
        return Physics::MakeSoftBodyHandle(GetGeneration(), index);
    }

    void WebGpuSoftBodyBackend::Destroy(const Physics::SoftBodyHandle handle)
    {
        if (Body *body = Find(handle))
        {
            ReleaseBuffers(*body);
            _bodies.erase(Physics::SoftBodyHandleIndex(handle));
        }
    }

    void WebGpuSoftBodyBackend::SetParams(const Physics::SoftBodyHandle handle, const Physics::SoftBodyParams &params)
    {
        if (Body *body = Find(handle))
        {
            // A new density scales every mass alike: the shader multiplies the inverse masses by massDensity / density
            body->params = Sanitized(params);
        }
    }

    void WebGpuSoftBodyBackend::AddVelocity(const Physics::SoftBodyHandle handle, const Math::Vector3 &velocity)
    {
        Body *body = Find(handle);
        if (!body || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
        {
            return;
        }
        body->push.x += velocity.x;
        body->push.y += velocity.y;
        body->push.z += velocity.z;
    }

    void WebGpuSoftBodyBackend::ResetToRest(Body &body)
    {
        const std::uint64_t nodeBytes = std::uint64_t{body.nodeCount} * 16;
        const std::vector<float> zeros(std::size_t{body.nodeCount} * 4, 0.0f);
        if (!_compute->WriteBuffer(body.positions, 0, body.restNodes.data(), nodeBytes) ||
            !_compute->WriteBuffer(body.velocities, 0, zeros.data(), nodeBytes))
        {
            _broken = true;
        }
        if (body.ticket != 0)
        {
            _compute->CancelRead(body.ticket); // it would bring back the positions from before the reset
            body.ticket = 0;
        }
        for (std::uint32_t i = 0; i < body.nodeCount; ++i)
        {
            body.nodes[i] = Math::Vector3(body.restNodes[i * 4], body.restNodes[i * 4 + 1], body.restNodes[i * 4 + 2]);
        }
        body.push = Math::Vector3{};
    }

    void WebGpuSoftBodyBackend::PollRead(Body &body) const
    {
        if (body.ticket == 0 || !_compute || _lifetime.expired())
        {
            return;
        }
        switch (_compute->PollRead(body.ticket, body.readBytes))
        {
        case GpuReadStatus::Pending:
            return;
        case GpuReadStatus::Done:
            if (body.readBytes.size() == std::size_t{body.nodeCount} * 16)
            {
                for (std::uint32_t i = 0; i < body.nodeCount; ++i)
                {
                    float node[4];
                    std::memcpy(node, body.readBytes.data() + std::size_t{i} * 16, sizeof(node));
                    body.nodes[i] = Math::Vector3(node[0], node[1], node[2]);
                }
            }
            break;
        case GpuReadStatus::Failed:
            break;
        }
        body.ticket = 0;
    }

    void WebGpuSoftBodyBackend::StepBody(Body &body, const float dt)
    {
        PollRead(body);
        if (!Usable())
        {
            return;
        }
        // A body that blew up (a non-finite position in the last copy) restarts from its rest shape, as on the CPU
        float check = 0.0f;
        for (const Math::Vector3 &p : body.nodes)
        {
            check += p.x + p.y + p.z;
        }
        if (!std::isfinite(check))
        {
            ResetToRest(body);
            if (!Usable())
            {
                return;
            }
        }

        const Physics::SoftBodyParams &params = body.params;
        const float h = dt / static_cast<float>(kSubSteps);
        const float h2 = h * h;
        const float shear = params.stiffness / (2.0f * (1.0f + params.poisson));
        const float bulk = params.stiffness / (3.0f * (1.0f - 2.0f * params.poisson));

        static_assert(sizeof(GpuParams) == 96, "must match the Params block of softbody.wgsl");
        GpuParams block{};
        block.gravity[0] = params.gravity.x;
        block.gravity[1] = params.gravity.y;
        block.gravity[2] = params.gravity.z;
        block.h = h;
        block.push[0] = body.push.x;
        block.push[1] = body.push.y;
        block.push[2] = body.push.z;
        block.keep = 1.0f / (1.0f + params.damping * h);
        block.nodeCount = body.nodeCount;
        block.edgeCount = body.edgeCount;
        block.tetCount = body.tetCount;
        block.subSteps = kSubSteps;
        block.iterations = params.solverIterations;
        block.edgeColours = body.edgeColours;
        block.tetColours = body.tetColours;
        block.groundEnabled = params.groundEnabled ? 1u : 0u;
        block.edgeAlphaBase = 1.0f / (shear * h2);
        block.volumeAlphaBase = 1.0f / (bulk * h2);
        block.groundHeight = params.groundHeight;
        block.friction = params.friction;
        block.invMassScale = body.massDensity / params.density;
        body.push = Math::Vector3{};

        const std::array<Renderer::Common::GpuBufferId, 8> buffers = {body.uniform, body.positions, body.previous,
                                                                      body.velocities, body.edges, body.tetBuffer,
                                                                      body.ranges, body.lambdas};
        bool ok = _compute->WriteBuffer(body.uniform, 0, &block, sizeof(block));
        ok = ok && _compute->Dispatch(_simulate, buffers, 1); // one workgroup runs the body's whole step
        if (ok && body.ticket == 0)
        {
            body.ticket = _compute->RequestRead(body.positions, 0, std::uint64_t{body.nodeCount} * 16);
            ok = body.ticket != 0;
        }
        else if (ok)
        {
            ok = _compute->Submit();
        }
        if (!ok)
        {
            _broken = true;
        }
    }

    void WebGpuSoftBodyBackend::Step(const float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }
        for (auto &entry : _bodies)
        {
            StepBody(entry.second, dt);
        }
    }

    void WebGpuSoftBodyBackend::CopySurfacePositions(const Physics::SoftBodyHandle handle,
                                                     std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (!Find(handle))
        {
            return;
        }
        Body &body = _bodies.at(Physics::SoftBodyHandleIndex(handle));
        PollRead(body);
        SkinSurface<Math::Vector3>(*body.mesh, body.tets, body.flipped, body.nodes, out);
    }

    void WebGpuSoftBodyBackend::CopyNodePositions(const Physics::SoftBodyHandle handle,
                                                  std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (!Find(handle))
        {
            return;
        }
        Body &body = _bodies.at(Physics::SoftBodyHandleIndex(handle));
        PollRead(body);
        out = body.nodes;
    }

    std::array<std::uint32_t, 2> WebGpuSoftBodyBackend::GetColourCounts(const Physics::SoftBodyHandle handle) const
    {
        const Body *body = Find(handle);
        return body ? std::array<std::uint32_t, 2>{body->edgeColours, body->tetColours}
                    : std::array<std::uint32_t, 2>{0, 0};
    }

    bool WebGpuSoftBodyBackend::SyncForTest()
    {
        if (!Usable() || !_compute->WaitIdleForTest())
        {
            return false;
        }
        for (auto &entry : _bodies)
        {
            Body &body = entry.second;
            PollRead(body); // lands the read in flight, which WaitIdleForTest completed
            body.ticket = _compute->RequestRead(body.positions, 0, std::uint64_t{body.nodeCount} * 16);
            if (body.ticket == 0 || !_compute->WaitIdleForTest())
            {
                return false;
            }
            PollRead(body);
            if (body.ticket != 0)
            {
                return false;
            }
        }
        return true;
    }
}
