#include "engine/rendering/ParticleSystem.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <renderer/common/IShader.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/particles/ParticleBillboards.hpp"
#include "engine/rendering/RenderCamera.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        // The backend's own random stream differs from the emitter's, so a lifetime never mirrors a speed
        constexpr std::uint32_t kBackendSeedMix = 0x5BD1E995u;

        // One warning per process for a renderer that can't draw particles, not one per frame or per system
        bool FirstRenderWarning()
        {
            static bool warned = false;
            return !std::exchange(warned, true);
        }
    }

    ParticleSystem::ParticleSystem(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();

        RegisterMember(NAMEOF(backend), backend)
            .Tooltip("Auto: PhysX on the GPU when the project enables it and it works, else CPU. PhysX: falls back to CPU, with a warning. CPU: the same on every machine. Read when the system starts.");
        RegisterMember(NAMEOF(playOnAwake), playOnAwake);
        RegisterMember(NAMEOF(maxParticles), maxParticles).Range(0.0, kMaxParticlesLimit)
            .Tooltip("The most particles alive at once; more are dropped. Read when the system starts.");
        RegisterMember(NAMEOF(seed), seed).Tooltip("The same seed gives the same particles. Read when the system starts.");

        RegisterMember(NAMEOF(emissionRate), emissionRate).Range(0.0, 100000.0).Tooltip("Particles per second while playing");
        RegisterMember(NAMEOF(burstCount), burstCount).Range(0.0, kMaxParticlesLimit)
            .Tooltip("Particles started at once by every Play");
        RegisterMember(NAMEOF(shape), shape);
        RegisterMember(NAMEOF(shapeRadius), shapeRadius).Range(0.0, 1000.0)
            .Tooltip("Radius of the cone's base, or of the sphere");
        RegisterMember(NAMEOF(coneAngle), coneAngle).Range(0.0, 180.0).Tooltip("Degrees, half angle of the cone");

        RegisterMember(NAMEOF(lifetimeMin), lifetimeMin).Range(0.001, 3600.0).Tooltip("Seconds");
        RegisterMember(NAMEOF(lifetimeMax), lifetimeMax).Range(0.001, 3600.0).Tooltip("Seconds");
        RegisterMember(NAMEOF(speedMin), speedMin).Tooltip("World units per second at the start");
        RegisterMember(NAMEOF(speedMax), speedMax).Tooltip("World units per second at the start");

        RegisterMember(NAMEOF(startSize), startSize).Range(0.0, 1000.0).Tooltip("Quad edge in world units, at birth");
        RegisterMember(NAMEOF(endSize), endSize).Range(0.0, 1000.0).Tooltip("Quad edge in world units, at death");
        RegisterMember(NAMEOF(startColor), startColor);
        RegisterMember(NAMEOF(endColor), endColor).Tooltip("Alpha 0 fades the particle out");

        RegisterMember(NAMEOF(gravityScale), gravityScale).Tooltip("Multiplies the world gravity; 0 floats");
        RegisterMember(NAMEOF(drag), drag).Range(0.0, 1000.0).Tooltip("Share of the velocity lost per second");
        RegisterMember(NAMEOF(collideWithGround), collideWithGround)
            .Tooltip("Bounce off the plane y = groundHeight; the only collision the CPU backend does");
        RegisterMember(NAMEOF(groundHeight), groundHeight);
        RegisterMember(NAMEOF(bounce), bounce).Range(0.0, 1.0).Tooltip("Share of its speed a particle keeps off the ground");

        RegisterMember(NAMEOF(blendMode), blendMode);
        RegisterAssetRef(NAMEOF(texture), texture);
    }

    ParticleSystem::~ParticleSystem()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its GameObject never
        // joined a scene) may outlive its renderer, so the renderer isn't called.
        ReleaseRenderResources(false);
        ReleaseSystem();
    }

    Physics::IParticleBackend *ParticleSystem::Backend() const
    {
        if (_cpuBackend)
        {
            return _cpuBackend.get();
        }
        if (_usingGpu)
        {
            if (auto *physics = Application::GetInstance().Get3DPhysicsBackend())
            {
                return physics->GetParticleBackend();
            }
        }
        return nullptr;
    }

    std::size_t ParticleSystem::GetAliveCount() const
    {
        const auto *sim = Backend();
        return sim && _handle != Physics::kInvalidParticleSystem ? sim->GetAliveCount(_handle) : 0;
    }

    void ParticleSystem::GetParticles(std::vector<Math::Vector4> &out) const
    {
        out.clear();
        if (const auto *sim = Backend(); sim && _handle != Physics::kInvalidParticleSystem)
        {
            sim->CopyPositions(_handle, out);
        }
    }

    std::string_view ParticleSystem::GetActiveBackendName() const
    {
        if (_handle == Physics::kInvalidParticleSystem)
        {
            return {};
        }
        return _usingGpu ? "PhysX" : "CPU";
    }

    Physics::ParticleParams ParticleSystem::BuildParams() const
    {
        Physics::ParticleParams params;
        Math::Vector3 gravity{0.0f, -9.81f, 0.0f};
        if (const auto *physics = Application::GetInstance().Get3DPhysicsBackend())
        {
            gravity = physics->GetGravity();
        }
        params.gravity = Math::Vector3(gravity.x * gravityScale, gravity.y * gravityScale, gravity.z * gravityScale);
        params.lifetimeMin = lifetimeMin;
        params.lifetimeMax = std::max(lifetimeMin, lifetimeMax);
        params.drag = drag;
        params.groundEnabled = collideWithGround;
        params.groundHeight = groundHeight;
        params.bounce = bounce;
        return params;
    }

    Particles::EmitterShapeSettings ParticleSystem::BuildShapeSettings() const
    {
        Particles::EmitterShapeSettings settings;
        settings.shape = shape;
        settings.shapeRadius = shapeRadius;
        settings.coneAngle = coneAngle;
        settings.speedMin = speedMin;
        settings.speedMax = std::max(speedMin, speedMax);
        return settings;
    }

    void ParticleSystem::OnAttach()
    {
        if (_handle != Physics::kInvalidParticleSystem)
        {
            return;
        }

        const Particles::BackendSelection selection =
            Particles::SelectParticleBackend(backend, Application::GetInstance().Get3DPhysicsBackend());
        if (selection.fellBack)
        {
            Logger::Warn(std::format("{} (GameObject '{}')", selection.reason, GetGameObject().GetName()));
        }

        Physics::ParticleSystemDesc desc;
        if (maxParticles > kMaxParticlesLimit)
        {
            Logger::Warn(std::format("ParticleSystem on '{}': maxParticles {} is above the limit of {}; using {}",
                                     GetGameObject().GetName(), maxParticles, kMaxParticlesLimit, kMaxParticlesLimit));
        }
        desc.maxParticles = static_cast<std::uint32_t>(std::clamp(maxParticles, 0, kMaxParticlesLimit));
        desc.seed = static_cast<std::uint32_t>(seed) ^ kBackendSeedMix;
        desc.params = BuildParams();

        if (selection.gpu)
        {
            _usingGpu = true;
            _handle = selection.gpu->CreateSystem(desc);
        }
        else
        {
            _usingGpu = false;
            _cpuBackend = std::make_unique<Particles::CpuParticleBackend>();
            _handle = _cpuBackend->CreateSystem(desc);
        }
        if (_handle == Physics::kInvalidParticleSystem)
        {
            Logger::Warn(std::format("ParticleSystem on '{}': the {} backend couldn't create the system; it emits nothing",
                                     GetGameObject().GetName(), _usingGpu ? "PhysX" : "CPU"));
            _cpuBackend.reset();
            _usingGpu = false;
            return;
        }
        Particles::ParticleUsage::Add(_usingGpu);

        _emitter.Reset(static_cast<std::uint32_t>(seed));
        _warnedFull = false;
        if (_pendingBurst > 0)
        {
            const int burst = std::exchange(_pendingBurst, 0);
            EmitBurst(burst);
        }
        // A script that called Play before the system started has its burst pending already
        if (playOnAwake && !_playing)
        {
            Play();
        }
    }

    void ParticleSystem::ReleaseSystem()
    {
        if (_handle != Physics::kInvalidParticleSystem)
        {
            if (auto *sim = Backend())
            {
                sim->Destroy(_handle);
            }
            Particles::ParticleUsage::Remove(_usingGpu);
        }
        _handle = Physics::kInvalidParticleSystem;
        _cpuBackend.reset();
        _usingGpu = false;
    }

    void ParticleSystem::OnDestroy()
    {
        CleanupRenderResources(_renderer);
        ReleaseSystem();
    }

    void ParticleSystem::OnFixedUpdate()
    {
        auto *sim = Backend();
        if (!sim || _handle == Physics::kInvalidParticleSystem)
        {
            return;
        }
        // The scaled fixed step: 0 while paused (time scale 0), so nothing moves and nothing is emitted
        const float dt = Time::GetFixedDeltaTime();
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }

        sim->SetParams(_handle, BuildParams());
        sim->Step(dt); // a backend that runs inside the physics scene ignores this
        if (_playing)
        {
            Emit(_emitter.Advance(emissionRate, dt));
        }
    }

    void ParticleSystem::Emit(const std::uint32_t count)
    {
        auto *sim = Backend();
        if (count == 0 || !sim || _handle == Physics::kInvalidParticleSystem)
        {
            return;
        }
        Math::Vector3 origin;
        Math::Quaternion rotation;
        if (const Positionable *positionable = GetGameObject().GetPositionable())
        {
            origin = positionable->GetPosition();
            rotation = positionable->GetRotation();
        }
        _emitter.Generate(BuildShapeSettings(), origin, rotation, count, _spawnPositions, _spawnVelocities);
        const std::size_t accepted = sim->Spawn(_handle, _spawnPositions, _spawnVelocities);
        if (accepted < count)
        {
            WarnFull();
        }
    }

    void ParticleSystem::WarnFull()
    {
        if (_warnedFull)
        {
            return;
        }
        _warnedFull = true;
        Logger::Warn(std::format(
            "ParticleSystem on '{}' reached maxParticles ({}): new particles are dropped until others expire. "
            "Raise maxParticles, or lower emissionRate or the lifetime.",
            GetGameObject().GetName(), std::min(maxParticles, kMaxParticlesLimit)));
    }

    void ParticleSystem::Play()
    {
        _playing = true;
        _emitter.ResetRate();
        if (burstCount > 0)
        {
            EmitBurst(burstCount);
        }
    }

    void ParticleSystem::Stop()
    {
        _playing = false;
    }

    void ParticleSystem::Clear()
    {
        _pendingBurst = 0;
        _warnedFull = false;
        if (auto *sim = Backend(); sim && _handle != Physics::kInvalidParticleSystem)
        {
            sim->Clear(_handle);
        }
    }

    void ParticleSystem::EmitBurst(const int count)
    {
        if (count <= 0)
        {
            return;
        }
        if (_handle == Physics::kInvalidParticleSystem)
        {
            // Not started (a script asked before OnAttach): it is emitted when the system starts
            _pendingBurst = static_cast<int>(std::min<long long>(static_cast<long long>(_pendingBurst) + count,
                                                                  kMaxParticlesLimit));
            return;
        }
        Emit(static_cast<std::uint32_t>(std::min(count, kMaxParticlesLimit)));
    }

    // ---------------------------------------------------------------- rendering

    bool ParticleSystem::HoldsRenderer(const Renderer::Common::IRenderer *renderer) const
    {
        return renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void ParticleSystem::BindRenderer(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || HoldsRenderer(renderer))
        {
            return;
        }
        // Everything held belongs to the previous renderer, which is only called if it still exists
        ReleaseRenderResources(true);
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();
    }

    void ParticleSystem::ReleaseRenderResources(bool callRenderer)
    {
        // A destroyed renderer freed everything itself, and its address may now be another renderer's
        callRenderer = callRenderer && _renderer && !_rendererLifetime.expired();
        if (callRenderer)
        {
            if (_mesh)
            {
                _renderer->DestroyMesh(_mesh);
            }
            if (_material)
            {
                _renderer->DestroyMaterial(_material);
            }
        }
        _textureHandle.Release(callRenderer);
        _mesh = nullptr;
        _material = nullptr;
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    void ParticleSystem::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            BindRenderer(renderer);
        }
    }

    void ParticleSystem::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _renderer)
        {
            return;
        }
        ReleaseRenderResources(true);
    }

    void ParticleSystem::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void ParticleSystem::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                       const RenderQueue queue)
    {
        static_cast<void>(queue); // it only draws in the Transparent queue
        const auto *sim = Backend();
        if (!renderer || !sim || _handle == Physics::kInvalidParticleSystem)
        {
            return;
        }

        sim->CopyPositions(_handle, _particles);
        if (_particles.empty())
        {
            return;
        }

        Renderer::Common::IShader *shader = renderer->GetStandardUnlitShader();
        if (!shader)
        {
            if (FirstRenderWarning())
            {
                Logger::Warn(std::format("ParticleSystem: the {} has no unlit shader, so particles aren't drawn",
                                         renderer->GetRendererName()));
            }
            return;
        }
        BindRenderer(renderer);

        // The texture: a share of the asset's GPU texture, none when the field is empty
        const bool textureChanged = texture.get() != _textureHandle.GetSource();
        if (textureChanged)
        {
            _textureHandle.Release(true);
            if (texture)
            {
                _textureHandle = GpuCache::AcquireTexture(*renderer, texture);
            }
            if (_material)
            {
                _material->SetTexture(_textureHandle.GetTexture());
            }
        }
        if (!_material)
        {
            _material = renderer->CreateMaterial(shader, _textureHandle.GetTexture());
            if (!_material)
            {
                if (FirstRenderWarning())
                {
                    Logger::Warn(std::format("ParticleSystem: the {} can't create the particle material, so particles "
                                             "aren't drawn", renderer->GetRendererName()));
                }
                return;
            }
            _material->SetColor("uAlbedo", 1.0f, 1.0f, 1.0f, 1.0f);
            // Fully faded quads are skipped (the software renderer, which can't blend, needs the cut)
            _material->SetFloat("uAlphaCutoff", 0.01f);
        }

        Particles::BillboardStyle style;
        style.startSize = startSize;
        style.endSize = endSize;
        style.startColor = startColor;
        style.endColor = endColor;
        Particles::BuildBillboards(_meshData, _particles, RenderCamera::Current(), style);

        // In place, or replaced where the backend can't update meshes
        if (_mesh && !renderer->UpdateMesh(_mesh, _meshData))
        {
            renderer->DestroyMesh(_mesh);
            _mesh = nullptr;
        }
        if (!_mesh)
        {
            _mesh = renderer->CreateMesh(_meshData);
            if (!_mesh)
            {
                if (FirstRenderWarning())
                {
                    Logger::Warn(std::format("ParticleSystem: the {} can't create the particle mesh, so particles "
                                             "aren't drawn", renderer->GetRendererName()));
                }
                return;
            }
        }

        // Billboards face the camera and aren't lit: two-sided, so a mirrored view still shows them
        Renderer::Common::RenderState drawState = state;
        drawState.cull = Renderer::Common::CullMode::None;
        drawState.additive = blendMode == ParticleBlend::Additive;
        renderer->DrawMesh(_mesh, kIdentity, _material, drawState);
    }
}
