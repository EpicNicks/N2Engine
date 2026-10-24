#include "engine/physics/SoftBody.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics/CpuSoftBodyBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

namespace N2Engine::Physics
{
    SoftBody::SoftBody(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        _gameObject.CreatePositionable();

        RegisterMember(NAMEOF(backend), backend)
            .Tooltip("Auto: PhysX (deformable volumes) on the GPU when the project enables physics.gpu and it works, else CPU. PhysX: falls back to CPU, with a warning. CPU: the same on every machine. Read when the body starts.");
        RegisterMember(NAMEOF(resolution), resolution).Range(kMinTetResolution, kMaxTetResolution)
            .Tooltip("Voxels along the longest axis of the mesh the body is built from: more is finer and slower. Read when the body starts.");
        RegisterMember(NAMEOF(density), density).Range(0.001, 1.0e7).Tooltip("Mass per unit volume in kg/m^3 (water is 1000)");
        RegisterMember(NAMEOF(stiffness), stiffness).Range(1.0, 1.0e11)
            .Tooltip("Young's modulus in pascals: how hard the body resists being deformed (a soft gel 1e4, rubber 1e6)");
        RegisterMember(NAMEOF(poisson), poisson).Range(0.0, 0.49)
            .Tooltip("Poisson's ratio: how much the body keeps its volume when squashed (0.49 is nearly incompressible)");
        RegisterMember(NAMEOF(damping), damping).Range(0.0, 1000.0)
            .Tooltip("Velocity decay in 1/s: how quickly the wobble dies out");
        RegisterMember(NAMEOF(friction), friction).Range(0.0, 1.0).Tooltip("Friction against the ground, 0 to 1");
        RegisterMember(NAMEOF(solverIterations), solverIterations).Range(1.0, CpuSoftBodyBackend::kMaxIterations)
            .Tooltip("Solver iterations in each of the CPU backend's 4 sub-steps: more is stiffer and slower");
        RegisterMember(NAMEOF(collideWithGround), collideWithGround)
            .Tooltip("Rest on the plane y = groundHeight; the only collision the CPU backend does (the PhysX backend collides with the scene's colliders instead)");
        RegisterMember(NAMEOF(groundHeight), groundHeight);
        RegisterMember(NAMEOF(materialModel), materialModel)
            .Tooltip("Used only by the PhysX backend: CoRotational is fast and needs well-shaped tetrahedra, NeoHookean is robust and slower. The CPU backend ignores it");
        RegisterMember(NAMEOF(selfCollision), selfCollision)
            .Tooltip("Used only by the PhysX backend: the body collides with itself. The CPU backend ignores it");
    }

    SoftBody::~SoftBody()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its GameObject never
        // joined a scene) may outlive its renderer component, so that isn't touched.
        ReleaseSim(false);
    }

    ISoftBodyBackend *SoftBody::Backend() const
    {
        if (_cpuBackend)
        {
            return _cpuBackend.get();
        }
        if (_usingGpu)
        {
            if (auto *physics = Application::GetInstance().Get3DPhysicsBackend())
            {
                // Only the backend that issued the handle knows it: the handle carries that backend's generation,
                // so a replaced or recreated backend (even one at the same address) is not it, and has other bodies
                auto *gpu = physics->GetSoftBodyBackend();
                return gpu && gpu->OwnsHandle(_handle) ? gpu : nullptr;
            }
        }
        return nullptr;
    }

    std::string_view SoftBody::GetActiveBackendName() const
    {
        if (_handle == kInvalidSoftBody)
        {
            return {};
        }
        return _usingGpu ? "PhysX" : "CPU";
    }

    SoftBodyParams SoftBody::BuildParams() const
    {
        SoftBodyParams params;
        params.density = density;
        params.stiffness = stiffness;
        params.poisson = poisson;
        params.damping = damping;
        params.friction = friction;
        params.solverIterations = static_cast<std::uint32_t>(std::max(solverIterations, 1));
        params.gravity = Math::Vector3(0.0f, -9.81f, 0.0f);
        if (const auto *physics = Application::GetInstance().Get3DPhysicsBackend())
        {
            params.gravity = physics->GetGravity();
        }
        params.groundEnabled = collideWithGround;
        params.groundHeight = groundHeight;
        params.materialModel = materialModel;
        params.selfCollision = selfCollision;
        return params;
    }

    SoftBodyDesc SoftBody::BuildDesc() const
    {
        SoftBodyDesc desc;
        desc.params = BuildParams();
        desc.tetMesh = _tetMesh;
        if (const Positionable *positionable = GetGameObject().GetPositionable())
        {
            const auto matrix = positionable->GetLocalToWorldMatrix();
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    desc.localToWorld[row * 4 + column] = matrix(row, column);
                }
            }
        }
        return desc;
    }

    void SoftBody::OnAttach()
    {
        _attached = true;
        if (_handle != kInvalidSoftBody)
        {
            return;
        }
        Start();
    }

    bool SoftBody::Start()
    {
        const std::string name = GetGameObject().GetName();

        auto *renderer = GetGameObject().GetComponent<Rendering::MeshRenderer>();
        if (!renderer)
        {
            Logger::Warn(std::format("SoftBody on '{}' needs a MeshRenderer on the same GameObject; it does nothing",
                                     name));
            return false;
        }
        if (GetGameObject().GetComponent<Rigidbody>())
        {
            Logger::Warn(std::format("SoftBody on '{}' can't share a GameObject with a Rigidbody (the Rigidbody would "
                                     "move the object while the soft body deforms its mesh); it does nothing", name));
            return false;
        }
        const std::shared_ptr<Rendering::Mesh> source = renderer->GetMesh();
        if (!source)
        {
            Logger::Warn(std::format("SoftBody on '{}': its MeshRenderer has no mesh; it does nothing", name));
            return false;
        }

        // 1. The tetrahedral mesh (cached per mesh version and resolution)
        const int requested = std::clamp(resolution, kMinTetResolution, kMaxTetResolution);
        if (requested != resolution)
        {
            Logger::Warn(std::format("SoftBody on '{}': resolution {} is outside {} to {}; using {}", name, resolution,
                                     kMinTetResolution, kMaxTetResolution, requested));
        }
        SoftBodies::TetResult tets = SoftBodies::TetCache::Instance().GetOrBuild(source, requested);
        if (!tets.Ok())
        {
            Logger::Warn(std::format("SoftBody on '{}': can't make a tetrahedral body from the mesh ({}); it does nothing",
                                     name, tets.error));
            return false;
        }
        if (tets.mesh->resolution < requested)
        {
            Logger::Warn(std::format("SoftBody on '{}': resolution {} would need more than {} tetrahedra; using {}",
                                     name, requested, kMaxTetsPerBody, tets.mesh->resolution));
        }
        _tetMesh = tets.mesh;

        // 2. The backend
        const SoftBodies::SoftBodyBackendSelection selection =
            SoftBodies::SelectSoftBodyBackend(backend, Application::GetInstance().Get3DPhysicsBackend());
        if (selection.fellBack)
        {
            Logger::Warn(std::format("{} (GameObject '{}')", selection.reason, name));
        }

        const SoftBodyDesc desc = BuildDesc();
        _usingGpu = false;
        if (selection.gpu)
        {
            _handle = selection.gpu->CreateBody(desc);
            if (_handle != kInvalidSoftBody)
            {
                _usingGpu = true;
            }
            else
            {
                // The GPU refused (out of memory, no GPU library, ...): the body starts on the CPU backend instead,
                // and the physics backend, which learns of the failure from its soft-body backend, stops offering
                // the GPU one
                Logger::Warn(std::format("SoftBody on '{}': the PhysX backend couldn't create the body; using the CPU "
                                         "backend", name));
                if (auto *physics = Application::GetInstance().Get3DPhysicsBackend())
                {
                    static_cast<void>(physics->GetSoftBodyBackend());
                }
            }
        }
        if (!_usingGpu)
        {
            _cpuBackend = std::make_unique<CpuSoftBodyBackend>();
            _handle = _cpuBackend->CreateBody(desc);
        }
        if (_handle == kInvalidSoftBody)
        {
            Logger::Warn(std::format("SoftBody on '{}': the {} backend couldn't create the body; it does nothing", name,
                                     _usingGpu ? "PhysX" : "CPU"));
            _cpuBackend.reset();
            _usingGpu = false;
            _tetMesh.reset();
            return false;
        }

        // 3. The picture: a runtime copy of the mesh that the MeshRenderer draws in place of its own
        _meshData = source->GetMeshData();
        const std::span<const Rendering::Submesh> submeshes = source->GetSubmeshes();
        _submeshes.assign(submeshes.begin(), submeshes.end());
        _runtimeMesh = Rendering::Mesh::Create(_meshData, _submeshes);
        if (!_runtimeMesh)
        {
            Logger::Warn(std::format("SoftBody on '{}': can't make the mesh it draws; it does nothing", name));
            ReleaseSim(false);
            return false;
        }
        renderer->SetDeformedMesh(_runtimeMesh);
        SoftBodies::SoftBodyUsage::Add(_usingGpu);
        return true;
    }

    void SoftBody::ReleaseSim(const bool touchRenderer)
    {
        if (_handle != kInvalidSoftBody)
        {
            if (auto *sim = Backend())
            {
                sim->Destroy(_handle);
            }
            // A body that never got as far as drawing counts nowhere (Start counts it last)
            if (_runtimeMesh)
            {
                SoftBodies::SoftBodyUsage::Remove(_usingGpu);
            }
        }
        if (touchRenderer && _runtimeMesh)
        {
            if (auto *renderer = GetGameObject().GetComponent<Rendering::MeshRenderer>();
                renderer && renderer->GetDeformedMesh() == _runtimeMesh)
            {
                renderer->ClearDeformedMesh();
            }
        }
        _handle = kInvalidSoftBody;
        _cpuBackend.reset();
        _usingGpu = false;
        _tetMesh.reset();
        _runtimeMesh.reset();
        _meshData = {};
        _submeshes.clear();
    }

    bool SoftBody::RecoverFromReplacedBackend()
    {
        // With no physics backend at all the body stays inert (documented); only a replacement is recovered from
        if (!_usingGpu || _handle == kInvalidSoftBody || !Application::GetInstance().Get3DPhysicsBackend() || Backend())
        {
            return false;
        }
        // The physics backend was replaced or removed: the handle belonged to the old one, which owns (or already
        // freed) its body, so it isn't destroyed here. The body restarts at rest on the CPU backend.
        Logger::Warn(std::format("SoftBody on '{}': the physics backend changed, so the PhysX body is dropped and the "
                                 "body continues on the CPU (it restarts at rest)", GetGameObject().GetName()));
        SoftBodies::SoftBodyUsage::Remove(true);
        _usingGpu = false;
        _cpuBackend = std::make_unique<CpuSoftBodyBackend>();
        _handle = _cpuBackend->CreateBody(BuildDesc());
        if (_handle == kInvalidSoftBody)
        {
            _cpuBackend.reset();
            // Nothing runs, and Start's count is already given back
            if (auto *renderer = GetGameObject().GetComponent<Rendering::MeshRenderer>();
                renderer && renderer->GetDeformedMesh() == _runtimeMesh)
            {
                renderer->ClearDeformedMesh();
            }
            _runtimeMesh.reset();
            return true;
        }
        SoftBodies::SoftBodyUsage::Add(false);
        return true;
    }

    void SoftBody::OnDestroy()
    {
        ReleaseSim(true);
        _attached = false;
    }

    void SoftBody::Reset()
    {
        if (!_attached)
        {
            return;
        }
        ReleaseSim(true);
        Start();
    }

    void SoftBody::AddVelocity(const Math::Vector3 &velocity)
    {
        if (auto *sim = Backend(); sim && _handle != kInvalidSoftBody)
        {
            sim->AddVelocity(_handle, velocity);
        }
    }

    void SoftBody::OnFixedUpdate()
    {
        RecoverFromReplacedBackend();
        auto *sim = Backend();
        if (!sim || _handle == kInvalidSoftBody)
        {
            return;
        }
        // The scaled fixed step: 0 while paused (time scale 0), so nothing moves
        const float dt = Time::GetFixedDeltaTime();
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }

        sim->SetParams(_handle, BuildParams());
        sim->Step(dt); // a backend that runs inside the physics scene ignores this
        UpdateRuntimeMesh();
    }

    void SoftBody::UpdateRuntimeMesh()
    {
        const auto *sim = Backend();
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!sim || _handle == kInvalidSoftBody || !_runtimeMesh || !positionable)
        {
            return;
        }
        sim->CopySurfacePositions(_handle, _worldPositions);
        auto &vertices = _meshData.vertices;
        if (_worldPositions.size() != vertices.size())
        {
            return;
        }

        // World to the object's local space, with its current transform
        const auto toLocal = positionable->GetWorldToLocalMatrix();
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            const Math::Vector3 &p = _worldPositions[i];
            vertices[i].position[0] = toLocal(0, 0) * p.x + toLocal(0, 1) * p.y + toLocal(0, 2) * p.z + toLocal(0, 3);
            vertices[i].position[1] = toLocal(1, 0) * p.x + toLocal(1, 1) * p.y + toLocal(1, 2) * p.z + toLocal(1, 3);
            vertices[i].position[2] = toLocal(2, 0) * p.x + toLocal(2, 1) * p.y + toLocal(2, 2) * p.z + toLocal(2, 3);
        }

        // Area-weighted normals: every triangle adds its (unnormalised) face normal to its three vertices. A vertex
        // that gets none (not in a triangle, or only degenerate ones) keeps the normal it had.
        _normalSums.assign(vertices.size() * 3, 0.0f);
        const auto &indices = _meshData.indices;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const float *a = vertices[indices[t]].position;
            const float *b = vertices[indices[t + 1]].position;
            const float *c = vertices[indices[t + 2]].position;
            const float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
            const float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
            const float nx = e1y * e2z - e1z * e2y;
            const float ny = e1z * e2x - e1x * e2z;
            const float nz = e1x * e2y - e1y * e2x;
            for (int corner = 0; corner < 3; ++corner)
            {
                float *sum = &_normalSums[static_cast<std::size_t>(indices[t + corner]) * 3];
                sum[0] += nx;
                sum[1] += ny;
                sum[2] += nz;
            }
        }
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            const float *sum = &_normalSums[i * 3];
            const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
            if (length > 1e-20f && std::isfinite(length))
            {
                vertices[i].normal[0] = sum[0] / length;
                vertices[i].normal[1] = sum[1] / length;
                vertices[i].normal[2] = sum[2] / length;
            }
        }

        // Bumps the version, so every renderer re-uploads the mesh on its next draw
        _runtimeMesh->SetData(_meshData, _submeshes);
    }
}
