#include "engine/particles/WebGpuParticleBackend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <utility>

#include <renderer/webgpu/WgslSources.hpp>

#include "engine/Logger.hpp"
#include "engine/particles/PbdDriver.hpp"

namespace N2Engine::Particles
{
    using Renderer::Common::GpuBindingKind;
    using Renderer::Common::GpuBufferUsage;
    using Renderer::Common::GpuReadStatus;

    /// The WGSL Params block of particles.wgsl, member for member (96 bytes, the vec3 first so the layouts agree)
    struct WebGpuParticleBackend::GpuParams
    {
        float gravity[3];
        float dt;
        std::uint32_t count;
        std::uint32_t hashMask;
        std::uint32_t cellCapacity;
        std::uint32_t granular;
        float radius;
        float invCell;
        float restDensity;
        float spacing;
        float keep;
        float friction;
        float tensile;
        float epsilon;
        std::uint32_t groundEnabled;
        float groundHeight;
        float bounce;
        float poly6;
        float spikyGrad;
        float invTensileW;
        float maxMove;
        float lifetimeStep;
    };

    namespace
    {
        constexpr std::uint32_t kWorkgroupSize = 64; // @workgroup_size of every kernel in particles.wgsl
        constexpr float kMinLifetime = 1e-3f;        // as the CPU backend: a lifetime of 0 would make a particle born dead
        constexpr std::size_t kVec4Bytes = 16;
        /// The artificial pressure of a fluid preset with surface tension, relative to the rest gradient (dimensionless)
        constexpr float kTensileStrength = 0.1f;
        /// The density constraint's relaxation, relative to the rest gradient
        constexpr float kRelaxation = 0.1f;

        // group 0: the Params block, then pos, vel, predicted, delta, cellCount, cellItems
        constexpr std::array<GpuBindingKind, 7> kBindings = {
            GpuBindingKind::Uniform, GpuBindingKind::Storage, GpuBindingKind::Storage, GpuBindingKind::Storage,
            GpuBindingKind::Storage, GpuBindingKind::Storage, GpuBindingKind::Storage};

        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        /// The CPU backend's sanitising, so both read a ParticleSystem's settings the same way
        Physics::ParticleParams Sanitized(Physics::ParticleParams params)
        {
            params.gravity.x = Finite(params.gravity.x, 0.0f);
            params.gravity.y = Finite(params.gravity.y, 0.0f);
            params.gravity.z = Finite(params.gravity.z, 0.0f);
            params.lifetimeMin = std::max(Finite(params.lifetimeMin, 1.0f), kMinLifetime);
            params.lifetimeMax = std::max(Finite(params.lifetimeMax, params.lifetimeMin), params.lifetimeMin);
            params.drag = std::max(Finite(params.drag, 0.0f), 0.0f);
            params.groundHeight = Finite(params.groundHeight, 0.0f);
            params.bounce = std::clamp(Finite(params.bounce, 0.0f), 0.0f, 1.0f);
            return params;
        }

        std::uint32_t NextPowerOfTwo(const std::uint32_t value)
        {
            std::uint32_t power = 1;
            while (power < value && power < (1u << 30))
            {
                power <<= 1;
            }
            return power;
        }

        std::uint32_t Groups(const std::uint32_t count)
        {
            return (count + kWorkgroupSize - 1) / kWorkgroupSize;
        }

        float Poly6Coefficient(const float h)
        {
            const float h3 = h * h * h;
            return 315.0f / (64.0f * std::numbers::pi_v<float> * h3 * h3 * h3); // integer powers, multiplied out
        }

        float SpikyGradCoefficient(const float h)
        {
            const float h3 = h * h * h;
            return -45.0f / (std::numbers::pi_v<float> * h3 * h3);
        }

        float Poly6(const float r2, const float h)
        {
            const float h2 = h * h;
            if (r2 >= h2)
            {
                return 0.0f;
            }
            const float d = h2 - r2;
            return Poly6Coefficient(h) * d * d * d;
        }

        /// The density of particles on a cubic lattice of `spacing` (what the fluid constraint keeps), and the sum of the
        /// squared constraint gradients of one particle there (what scales the relaxation and the artificial pressure)
        void RestLattice(const float spacing, const float h, float &density, float &gradientSq)
        {
            density = 0.0f;
            const int reach = static_cast<int>(std::ceil(h / spacing));
            for (int z = -reach; z <= reach; ++z)
            {
                for (int y = -reach; y <= reach; ++y)
                {
                    for (int x = -reach; x <= reach; ++x)
                    {
                        const float r2 = spacing * spacing * static_cast<float>(x * x + y * y + z * z);
                        density += Poly6(r2, h);
                    }
                }
            }
            density = std::max(density, 1e-12f);
            gradientSq = 0.0f;
            const float spiky = SpikyGradCoefficient(h);
            for (int z = -reach; z <= reach; ++z)
            {
                for (int y = -reach; y <= reach; ++y)
                {
                    for (int x = -reach; x <= reach; ++x)
                    {
                        const float r = spacing * std::sqrt(static_cast<float>(x * x + y * y + z * z));
                        if (r <= 0.0f || r >= h)
                        {
                            continue;
                        }
                        // |grad W| / rest density: the spiky gradient's length is |spiky| (h - r)^2
                        const float g = std::abs(spiky) * (h - r) * (h - r) / density;
                        gradientSq += g * g;
                    }
                }
            }
            gradientSq = std::max(gradientSq, 1e-12f);
        }
    }

    WebGpuParticleBackend::WebGpuParticleBackend(Renderer::Common::IGpuCompute *compute, std::weak_ptr<const void> lifetime)
        : _compute(compute), _lifetime(std::move(lifetime))
    {
        if (!Usable() || !MakeKernels())
        {
            MarkBroken();
        }
    }

    WebGpuParticleBackend::~WebGpuParticleBackend()
    {
        for (auto &entry : _systems)
        {
            ReleaseBuffers(entry.second);
        }
    }

    bool WebGpuParticleBackend::Usable() const
    {
        return !_broken && _compute != nullptr && !_lifetime.expired() && _compute->IsUsable();
    }

    bool WebGpuParticleBackend::IsHealthy() const
    {
        return Usable();
    }

    void WebGpuParticleBackend::MarkBroken()
    {
        _broken = true;
    }

    bool WebGpuParticleBackend::MakeKernels()
    {
        const std::string_view wgsl = Renderer::WebGpu::Wgsl::kParticlesComputeWgsl;
        const auto make = [&](const char *entry) {
            return _compute->CreateKernel("N2Engine particles", wgsl, entry, kBindings);
        };
        _clearCells = make("clear_cells");
        _predict = make("predict");
        _lambdas = make("lambdas");
        _deltas = make("deltas");
        _apply = make("apply");
        _finalize = make("finalize");
        return _clearCells && _predict && _lambdas && _deltas && _apply && _finalize;
    }

    WebGpuParticleBackend::System *WebGpuParticleBackend::Find(const Physics::ParticleSystemHandle handle)
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _systems.find(Physics::ParticleHandleIndex(handle));
        return it == _systems.end() ? nullptr : &it->second;
    }

    const WebGpuParticleBackend::System *WebGpuParticleBackend::Find(const Physics::ParticleSystemHandle handle) const
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _systems.find(Physics::ParticleHandleIndex(handle));
        return it == _systems.end() ? nullptr : &it->second;
    }

    void WebGpuParticleBackend::ReleaseBuffers(System &system)
    {
        // The compute belongs to the renderer: once its lifetime token expired, its buffers are gone with it
        if (_compute && !_lifetime.expired())
        {
            if (system.ticket != 0)
            {
                _compute->CancelRead(system.ticket);
            }
            for (const Renderer::Common::GpuBufferId buffer : {system.uniform, system.positions, system.velocities,
                                                               system.predicted, system.deltas, system.cellCounts,
                                                               system.cellItems})
            {
                if (buffer != 0)
                {
                    _compute->DestroyBuffer(buffer);
                }
            }
        }
        system.ticket = 0;
        system.uniform = system.positions = system.velocities = system.predicted = system.deltas = 0;
        system.cellCounts = system.cellItems = 0;
    }

    Physics::ParticleSystemHandle WebGpuParticleBackend::CreateSystem(const Physics::ParticleSystemDesc &desc)
    {
        if (!Usable())
        {
            return Physics::kInvalidParticleSystem;
        }
        System system;
        system.params = Sanitized(desc.params);
        system.capacity = std::min(desc.maxParticles, kMaxParticlesLimit);
        system.random.Seed(desc.seed);

        const PbdPreset preset = GetPbdPreset(desc.material);
        system.granular = !preset.fluid;
        system.spacing = std::max(preset.particleSpacing, 1e-3f);
        system.radius = 2.0f * system.spacing;
        system.friction = std::clamp(preset.material.friction, 0.0f, 1.0f);
        system.damping = std::max(preset.material.damping, 0.0f);
        RestLattice(system.spacing, system.radius, system.restDensity, system.restGradientSq);
        system.tensile = preset.fluid && preset.material.surfaceTension > 0.0f
                             ? kTensileStrength / system.restGradientSq
                             : 0.0f;

        const std::uint32_t slots = std::max(system.capacity, 1u);
        system.hashBuckets = NextPowerOfTwo(std::max(64u, 2 * slots));
        const std::uint64_t slotBytes = static_cast<std::uint64_t>(slots) * kVec4Bytes;
        system.uniform = _compute->CreateBuffer(GpuBufferUsage::Uniform, sizeof(GpuParams));
        system.positions = _compute->CreateBuffer(GpuBufferUsage::Storage, slotBytes);
        system.velocities = _compute->CreateBuffer(GpuBufferUsage::Storage, slotBytes);
        system.predicted = _compute->CreateBuffer(GpuBufferUsage::Storage, slotBytes);
        system.deltas = _compute->CreateBuffer(GpuBufferUsage::Storage, slotBytes);
        system.cellCounts = _compute->CreateBuffer(GpuBufferUsage::Storage, std::uint64_t{system.hashBuckets} * 4);
        system.cellItems =
            _compute->CreateBuffer(GpuBufferUsage::Storage, std::uint64_t{system.hashBuckets} * kCellCapacity * 4);
        if (!system.uniform || !system.positions || !system.velocities || !system.predicted || !system.deltas ||
            !system.cellCounts || !system.cellItems)
        {
            // Too big for the device, or out of memory: this system goes to the CPU, the backend stays usable
            ReleaseBuffers(system);
            return Physics::kInvalidParticleSystem;
        }

        system.remaining.assign(system.capacity, 0.0f);
        system.lifetime.assign(system.capacity, 0.0f);
        system.spawnPosition.assign(system.capacity, Math::Vector3{});
        system.spawnVelocity.assign(system.capacity, Math::Vector3{});
        system.spawnStep.assign(system.capacity, 0);

        const std::uint32_t index = _nextIndex++;
        _systems.emplace(index, std::move(system));
        return Physics::MakeParticleHandle(GetGeneration(), index);
    }

    void WebGpuParticleBackend::Destroy(const Physics::ParticleSystemHandle handle)
    {
        if (System *system = Find(handle))
        {
            ReleaseBuffers(*system);
            _systems.erase(Physics::ParticleHandleIndex(handle));
        }
    }

    void WebGpuParticleBackend::SetParams(const Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params)
    {
        if (System *system = Find(handle))
        {
            system->params = Sanitized(params);
        }
    }

    std::uint32_t WebGpuParticleBackend::TakeSlot(System &system)
    {
        // The lowest dead slot below the high-water mark, so the live ones stay packed at the start
        while (!system.freeSlots.empty())
        {
            std::ranges::pop_heap(system.freeSlots, std::greater<>{});
            const std::uint32_t slot = system.freeSlots.back();
            system.freeSlots.pop_back();
            if (slot < system.highWater && !(system.remaining[slot] > 0.0f))
            {
                return slot;
            }
            // a stale entry: the mark fell below it, or it was handed out again from above the mark
        }
        return system.highWater < system.capacity ? system.highWater++ : system.capacity;
    }

    void WebGpuParticleBackend::FreeSlot(System &system, const std::uint32_t slot)
    {
        system.remaining[slot] = 0.0f;
        system.freeSlots.push_back(slot);
        std::ranges::push_heap(system.freeSlots, std::greater<>{});
        --system.alive;
    }

    std::size_t WebGpuParticleBackend::Spawn(const Physics::ParticleSystemHandle handle,
                                             const std::span<const Math::Vector3> positions,
                                             const std::span<const Math::Vector3> velocities)
    {
        System *found = Find(handle);
        if (!found || !Usable())
        {
            return 0;
        }
        System &system = *found;
        const std::size_t room = system.capacity - system.alive;
        const std::size_t count = std::min({positions.size(), velocities.size(), room});
        if (count == 0)
        {
            return 0;
        }

        // The new particles by slot, so neighbouring slots go to the GPU in one write each
        struct NewParticle
        {
            std::uint32_t slot;
            std::array<float, 4> position;
            std::array<float, 4> velocity;
        };
        std::vector<NewParticle> added;
        added.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint32_t slot = TakeSlot(system);
            if (slot >= system.capacity)
            {
                break;
            }
            const Math::Vector3 &p = positions[i];
            const Math::Vector3 &v = velocities[i];
            const float lifetime = system.random.Range(system.params.lifetimeMin, system.params.lifetimeMax);
            system.remaining[slot] = lifetime;
            system.lifetime[slot] = lifetime;
            system.spawnPosition[slot] = p;
            system.spawnVelocity[slot] = v;
            system.spawnStep[slot] = system.steps;
            ++system.alive;
            added.push_back({slot, {p.x, p.y, p.z, lifetime}, {v.x, v.y, v.z, 0.0f}});
        }
        std::ranges::sort(added, {}, &NewParticle::slot);

        std::vector<float> positionRun;
        std::vector<float> velocityRun;
        std::size_t start = 0;
        while (start < added.size())
        {
            std::size_t end = start + 1;
            while (end < added.size() && added[end].slot == added[end - 1].slot + 1)
            {
                ++end;
            }
            positionRun.clear();
            velocityRun.clear();
            for (std::size_t i = start; i < end; ++i)
            {
                positionRun.insert(positionRun.end(), added[i].position.begin(), added[i].position.end());
                velocityRun.insert(velocityRun.end(), added[i].velocity.begin(), added[i].velocity.end());
            }
            const std::uint64_t offset = std::uint64_t{added[start].slot} * kVec4Bytes;
            const std::uint64_t bytes = positionRun.size() * sizeof(float);
            if (!_compute->WriteBuffer(system.positions, offset, positionRun.data(), bytes) ||
                !_compute->WriteBuffer(system.velocities, offset, velocityRun.data(), bytes))
            {
                MarkBroken();
                break;
            }
            start = end;
        }
        return added.size();
    }

    void WebGpuParticleBackend::Clear(const Physics::ParticleSystemHandle handle)
    {
        System *system = Find(handle);
        if (!system)
        {
            return;
        }
        if (system->highWater > 0 && Usable())
        {
            // Every slot used so far gets lifetime 0 (empty), so the GPU skips them
            const std::vector<float> zeros(std::size_t{system->highWater} * 4, 0.0f);
            if (!_compute->WriteBuffer(system->positions, 0, zeros.data(), zeros.size() * sizeof(float)))
            {
                MarkBroken();
            }
        }
        std::ranges::fill(system->remaining, 0.0f);
        system->freeSlots.clear();
        system->highWater = 0;
        system->alive = 0;
        if (system->ticket != 0 && _compute && !_lifetime.expired())
        {
            _compute->CancelRead(system->ticket);
        }
        system->ticket = 0;
        system->mirror.clear();
        system->mirrorStep = 0;
    }

    bool WebGpuParticleBackend::WriteParams(System &system, const float h) const
    {
        static_assert(sizeof(GpuParams) == 96, "must match the Params block of particles.wgsl");
        const Physics::ParticleParams &p = system.params;
        GpuParams block{};
        block.gravity[0] = p.gravity.x;
        block.gravity[1] = p.gravity.y;
        block.gravity[2] = p.gravity.z;
        block.dt = h;
        block.count = system.highWater;
        block.hashMask = system.hashBuckets - 1;
        block.cellCapacity = kCellCapacity;
        block.granular = system.granular ? 1u : 0u;
        block.radius = system.radius;
        block.invCell = 1.0f / system.radius;
        block.restDensity = system.restDensity;
        block.spacing = system.spacing;
        block.keep = std::max(0.0f, 1.0f - (p.drag + system.damping) * h);
        block.friction = system.friction;
        block.tensile = system.tensile;
        block.epsilon = kRelaxation * system.restGradientSq;
        block.groundEnabled = p.groundEnabled ? 1u : 0u;
        block.groundHeight = p.groundHeight;
        block.bounce = p.bounce;
        block.poly6 = Poly6Coefficient(system.radius);
        block.spikyGrad = SpikyGradCoefficient(system.radius);
        const float reference = Poly6(0.04f * system.radius * system.radius, system.radius); // W(0.2 h)
        block.invTensileW = reference > 0.0f ? 1.0f / reference : 0.0f;
        block.maxMove = 0.5f * system.spacing;
        block.lifetimeStep = h;
        return _compute->WriteBuffer(system.uniform, 0, &block, sizeof(block));
    }

    void WebGpuParticleBackend::PollRead(System &system) const
    {
        if (system.ticket == 0 || !_compute || _lifetime.expired())
        {
            return;
        }
        switch (_compute->PollRead(system.ticket, system.readBytes))
        {
        case GpuReadStatus::Pending:
            return;
        case GpuReadStatus::Done:
            // Field by field: Math::Vector4 keeps its components in the order w, x, y, z, not the GPU's x, y, z, w
            system.mirror.resize(system.readBytes.size() / kVec4Bytes);
            for (std::size_t i = 0; i < system.mirror.size(); ++i)
            {
                float slot[4];
                std::memcpy(slot, system.readBytes.data() + i * kVec4Bytes, sizeof(slot));
                Math::Vector4 &m = system.mirror[i];
                m.x = slot[0];
                m.y = slot[1];
                m.z = slot[2];
                m.w = slot[3];
            }
            system.mirrorStep = system.ticketStep;
            break;
        case GpuReadStatus::Failed:
            break;
        }
        system.ticket = 0;
    }

    void WebGpuParticleBackend::StepSystem(System &system, const float dt)
    {
        PollRead(system);
        if (!Usable())
        {
            return;
        }
        if (_maxStepsInFlight != 0 && system.ticket != 0 && system.steps - system.ticketStep >= _maxStepsInFlight)
        {
            // The GPU has not caught up with the steps already submitted: skip this one whole (the lifetimes too, so the
            // CPU's count stays the GPU's), rather than queue more work behind it
            ++_skippedSteps;
            return;
        }
        const float h = dt / static_cast<float>(kSubSteps);
        const std::uint32_t count = system.highWater; // every slot that may be alive at the start of the step
        if (count > 0)
        {
            if (!WriteParams(system, h))
            {
                MarkBroken();
                return;
            }
            const std::array<Renderer::Common::GpuBufferId, 7> buffers = {
                system.uniform, system.positions, system.velocities, system.predicted,
                system.deltas, system.cellCounts, system.cellItems};
            const std::uint32_t groups = Groups(count);
            const std::uint32_t bucketGroups = Groups(system.hashBuckets);
            bool ok = true;
            for (int sub = 0; sub < kSubSteps && ok; ++sub)
            {
                ok = ok && _compute->Dispatch(_clearCells, buffers, bucketGroups);
                ok = ok && _compute->Dispatch(_predict, buffers, groups);
                for (int iteration = 0; iteration < kIterations && ok; ++iteration)
                {
                    if (!system.granular)
                    {
                        ok = ok && _compute->Dispatch(_lambdas, buffers, groups);
                    }
                    ok = ok && _compute->Dispatch(_deltas, buffers, groups);
                    ok = ok && _compute->Dispatch(_apply, buffers, groups);
                }
                ok = ok && _compute->Dispatch(_finalize, buffers, groups);
            }
            ++system.steps;
            if (ok && system.ticket == 0)
            {
                system.ticket = _compute->RequestRead(system.positions, 0, std::uint64_t{count} * kVec4Bytes);
                system.ticketStep = system.steps;
                ok = system.ticket != 0;
            }
            else if (ok)
            {
                ok = _compute->Submit();
            }
            if (!ok)
            {
                MarkBroken();
                return;
            }
        }
        else
        {
            ++system.steps;
        }

        // The lifetimes, counted down exactly as finalize does on the GPU (a sub-step at a time, never past death), so
        // the CPU knows which slots the GPU still simulates without asking it
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            if (!(system.remaining[slot] > 0.0f))
            {
                continue;
            }
            for (int sub = 0; sub < kSubSteps && system.remaining[slot] > 0.0f; ++sub)
            {
                system.remaining[slot] = system.remaining[slot] - h;
            }
            if (!(system.remaining[slot] > 0.0f))
            {
                FreeSlot(system, slot);
            }
        }
        while (system.highWater > 0 && !(system.remaining[system.highWater - 1] > 0.0f))
        {
            --system.highWater;
        }
    }

    void WebGpuParticleBackend::Step(const float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }
        for (auto &entry : _systems)
        {
            StepSystem(entry.second, dt);
        }
    }

    std::size_t WebGpuParticleBackend::GetAliveCount(const Physics::ParticleSystemHandle handle) const
    {
        const System *system = Find(handle);
        return system ? system->alive : 0;
    }

    void WebGpuParticleBackend::CopyPositions(const Physics::ParticleSystemHandle handle,
                                              std::vector<Math::Vector4> &out) const
    {
        out.clear();
        const System *found = Find(handle);
        if (!found)
        {
            return;
        }
        System &system = _systems.at(Physics::ParticleHandleIndex(handle));
        PollRead(system);
        out.reserve(system.alive);
        const Math::Vector3 &g = system.params.gravity;
        for (std::uint32_t slot = 0; slot < system.highWater; ++slot)
        {
            if (!(system.remaining[slot] > 0.0f))
            {
                continue;
            }
            const float lifetime = system.lifetime[slot];
            const float age = std::max(0.0f, lifetime - system.remaining[slot]);
            Math::Vector4 &v = out.emplace_back();
            if (system.mirrorStep > system.spawnStep[slot] && slot < system.mirror.size())
            {
                // The GPU's position as of the last copy that landed
                v.x = system.mirror[slot].x;
                v.y = system.mirror[slot].y;
                v.z = system.mirror[slot].z;
            }
            else
            {
                // Spawned after that copy was taken: where it would be on its ballistic path
                const Math::Vector3 &p = system.spawnPosition[slot];
                const Math::Vector3 &u = system.spawnVelocity[slot];
                const float half = 0.5f * age * age;
                v.x = p.x + u.x * age + g.x * half;
                v.y = p.y + u.y * age + g.y * half;
                v.z = p.z + u.z * age + g.z * half;
                if (system.params.groundEnabled && v.y < system.params.groundHeight)
                {
                    v.y = system.params.groundHeight;
                }
            }
            v.w = std::clamp(age / lifetime, 0.0f, 1.0f);
        }
    }

    bool WebGpuParticleBackend::SyncForTest()
    {
        if (!Usable() || !_compute->WaitIdleForTest())
        {
            return false;
        }
        for (auto &entry : _systems)
        {
            System &system = entry.second;
            PollRead(system); // lands the read in flight, which WaitIdleForTest completed
            if (system.highWater == 0)
            {
                continue;
            }
            system.ticket = _compute->RequestRead(system.positions, 0, std::uint64_t{system.highWater} * kVec4Bytes);
            system.ticketStep = system.steps + 1; // as of every step so far, including a spawn after the last one
            if (system.ticket == 0 || !_compute->WaitIdleForTest())
            {
                return false;
            }
            PollRead(system);
            if (system.ticket != 0)
            {
                return false;
            }
        }
        return true;
    }
}
