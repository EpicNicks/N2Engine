#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/particles/ParticleEmitter.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Texture.hpp"

namespace N2Engine::Particles
{
    class CpuParticleBackend;
}

namespace N2Engine::Rendering
{
    /// How a ParticleSystem's billboards combine with what is behind them
    enum class ParticleBlend
    {
        /// Mixed by alpha (smoke, dust)
        Alpha,
        /// Added to the picture (fire, sparks, glows). Drawn as Alpha on the software renderer, which can't blend.
        Additive
    };

    N2_SERIALIZE_ENUM(ParticleBlend, {
                      { ParticleBlend::Alpha, "Alpha" },
                      { ParticleBlend::Additive, "Additive" }
                      })

    /**
     * Emits and draws particles: small camera-facing quads that start at the object, fly under gravity and fade
     * away. See docs/particles.html.
     *
     * Simulation: a ParticleSystem gets its particles simulated by an IParticleBackend, chosen by `backend`
     * when the component attaches (Auto, PhysX or CPU, see Particles::SelectParticleBackend). That is the CPU
     * backend unless the physics backend has a PhysX GPU particle backend (physics.gpu is auto or on and the GPU
     * works): Auto then uses the GPU one and otherwise the CPU one quietly, PhysX the GPU one and otherwise the CPU one
     * with one warning. A system on the GPU backend whose backend is replaced or recreated (its handle's generation
     * no longer matches the backend's) restarts on the CPU with one warning; with the physics backend removed
     * entirely it is inert. The emitter (rate, burst,
     * shape, speed) lives in the component and runs in OnFixedUpdate, once per fixed step, so a system emits and
     * moves the same particles whatever the frame rate; the same seed gives the same particles. Particles are
     * simulated in world space: moving the object moves where new ones start, not those already flying.
     *
     * Play mode: particles exist only while the scene is playing (nothing is simulated or drawn in a scene
     * opened for editing), they are never saved, and leaving play mode destroys them with the scene. Pausing
     * (time scale 0) freezes them: no fixed step runs, and the picture stays.
     *
     * Rendering: each frame the quads are rebuilt on the CPU from the backend's positions
     * (Particles::BuildBillboards) and drawn as one mesh updated in place (IRenderer::UpdateMesh) in the
     * Transparent queue with the unlit shader: the particle's colour (a vertex colour) times the optional texture.
     * That rebuild (4 vertices per particle) is the one per-frame cost beyond the simulation; the vectors keep their
     * capacity, so a steady system allocates nothing. Needs a renderer that creates meshes and materials (OpenGL and
     * the software renderer); on the Vulkan renderer, which is still stubs, nothing is drawn and one warning is
     * logged per process.
     */
    class ParticleSystem final : public IRenderable
    {
    public:
        /// maxParticles above this is lowered to it (with a warning) when the system starts
        static constexpr int kMaxParticlesLimit = 100000;

        explicit ParticleSystem(GameObject &gameObject);
        ~ParticleSystem() override;

        [[nodiscard]] std::string GetTypeName() const override { return "ParticleSystem"; }

        // Lifecycle
        void OnAttach() override;
        void OnFixedUpdate() override;
        void OnDestroy() override;

        // IRenderable
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override { return {RenderQueue::Transparent, 0}; }
        void Render(Renderer::Common::IRenderer *renderer) override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;

        // Control
        /// Starts emitting (the rate, and the burst once). A system that isn't running yet (not attached) starts when it is.
        void Play();
        /// Stops emitting; the particles already flying finish their lives
        void Stop();
        /// Removes every particle (emission is unaffected)
        void Clear();
        /// Starts `count` particles now, playing or not. Dropped (with the maxParticles warning) beyond room.
        void EmitBurst(int count);
        [[nodiscard]] bool IsPlaying() const { return _playing; }
        /// Living particles (0 before the system started)
        [[nodiscard]] std::size_t GetAliveCount() const;
        /// Replaces `out` with the living particles, one Vector4 each: x, y, z the world position, w the age as a
        /// fraction of the lifetime (IParticleBackend::CopyPositions). Empty before the system started.
        void GetParticles(std::vector<Math::Vector4> &out) const;
        /// "CPU" or "PhysX": the backend simulating this system; empty before it started (not attached)
        [[nodiscard]] std::string_view GetActiveBackendName() const;

        /// The simulation settings the current fields give (gravityScale applied to the world gravity)
        [[nodiscard]] Physics::ParticleParams BuildParams() const;
        /// The emitter shape settings the current fields give
        [[nodiscard]] Particles::EmitterShapeSettings BuildShapeSettings() const;

        static constexpr bool IsSingleton = false;

        // Settings, all saved with the scene (an inspector edits them; most apply at once, backend, maxParticles and
        // seed when the system starts)

        /// Which simulation to ask for; see Particles::ParticleBackend
        Particles::ParticleBackend backend = Particles::ParticleBackend::Auto;
        /// Starts emitting when the component attaches (play mode starts)
        bool playOnAwake = true;
        /// The most particles alive at once (at most kMaxParticlesLimit); more are dropped, with one warning
        int maxParticles = 1000;
        /// Seeds the random choices: the same seed gives the same particles
        int seed = 1;
        /// What the particles are made of on the PhysX backend (water, sand or smoke: a set of PBD material
        /// parameters); the CPU backend ignores it. Read when the system starts.
        Physics::ParticleMaterial pbdMaterial = Physics::ParticleMaterial::Water;

        // Emission
        /// Particles per second while playing
        float emissionRate = 20.0f;
        /// Particles started at once by every Play (and by playOnAwake)
        int burstCount = 0;
        Particles::EmitterShape shape = Particles::EmitterShape::Cone;
        /// Radius of the cone's base disc, or of the sphere, in world units
        float shapeRadius = 0.0f;
        /// Half angle of the cone, in degrees
        float coneAngle = 25.0f;

        // Start values: each particle's lifetime (seconds) and start speed are picked from a range
        float lifetimeMin = 1.0f;
        float lifetimeMax = 2.0f;
        float speedMin = 2.0f;
        float speedMax = 4.0f;

        // Over a particle's life, from its start to its end
        float startSize = 0.2f;
        float endSize = 0.05f;
        Common::Color startColor{Common::Color::White};
        /// Alpha 0 here fades the particle out
        Common::Color endColor{1.0f, 1.0f, 1.0f, 0.0f};

        // Motion
        /// Multiplies the world gravity (the physics backend's, else 0, -9.81, 0); 0 floats
        float gravityScale = 1.0f;
        /// Velocity lost per second, as a fraction of it
        float drag = 0.0f;
        /// Particles bounce off the plane y = groundHeight (world space). The only collision the CPU backend does.
        bool collideWithGround = false;
        float groundHeight = 0.0f;
        /// The share of its speed a particle keeps when it hits the ground, 0 to 1
        float bounce = 0.3f;

        // Drawing
        ParticleBlend blendMode = ParticleBlend::Alpha;
        /// Optional picture on every quad (its alpha times the particle's); none draws plain colours
        std::shared_ptr<Texture> texture;

    private:
        void ReleaseSystem();
        /// A PhysX-backed system whose physics backend was replaced drops its stale handle and restarts on the CPU
        /// backend with one warning; true when it did
        bool RecoverFromReplacedBackend();
        /// The backend simulating this system: the CPU one this component owns, or the physics backend's GPU one,
        /// asked for each time (it goes away with the physics backend); nullptr when there is none
        [[nodiscard]] Physics::IParticleBackend *Backend() const;
        /// Spawns `count` particles from the emitter at the object's current pose; the number that fit
        void Emit(std::uint32_t count);
        void WarnFull();
        void ReleaseRenderResources(bool callRenderer);
        [[nodiscard]] bool HoldsRenderer(const Renderer::Common::IRenderer *renderer) const;
        void BindRenderer(Renderer::Common::IRenderer *renderer);

        // The simulation
        std::unique_ptr<Particles::CpuParticleBackend> _cpuBackend; // owned when the CPU backend is the one in use
        Physics::ParticleSystemHandle _handle = Physics::kInvalidParticleSystem;
        bool _usingGpu = false;
        Particles::ParticleEmitter _emitter;
        bool _playing = false;
        int _pendingBurst = 0; // asked for before the system started
        bool _warnedFull = false;
        // Reused every step, so emitting allocates nothing once they have grown
        std::vector<Math::Vector3> _spawnPositions;
        std::vector<Math::Vector3> _spawnVelocities;

        // The picture
        std::vector<Math::Vector4> _particles;
        Renderer::Common::MeshData _meshData;
        Renderer::Common::IRenderer *_renderer = nullptr;
        std::weak_ptr<const void> _rendererLifetime; // expired once _renderer is destroyed
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        GpuCache::Handle _textureHandle;
    };
}
