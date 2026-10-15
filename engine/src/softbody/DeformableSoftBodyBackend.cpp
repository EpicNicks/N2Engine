#include "engine/softbody/DeformableSoftBodyBackend.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "engine/Logger.hpp"
#include "engine/softbody/TetSkinning.hpp"

namespace N2Engine::SoftBodies
{
    namespace
    {
        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        // The same ranges the CPU backend clamps to
        constexpr float kMinDensity = 1e-3f;
        constexpr float kMaxDensity = 1e7f;
        constexpr float kMinStiffness = 1.0f;
        constexpr float kMaxStiffness = 1e11f;
        constexpr std::uint32_t kMaxPositionIterations = 255; // PhysX: [1, 255]

        bool MeshIsUsable(const Physics::TetMeshData &mesh)
        {
            if (mesh.positions.empty() || mesh.tets.empty() || mesh.tets.size() > Physics::kMaxTetsPerBody)
            {
                return false;
            }
            for (const auto &tet : mesh.tets)
            {
                for (const std::uint32_t node : tet)
                {
                    if (node >= mesh.positions.size())
                    {
                        return false;
                    }
                }
            }
            return std::ranges::all_of(mesh.surfaceEmbedding,
                                       [&](const Physics::TetEmbedding &e) { return e.tet < mesh.tets.size(); });
        }
    }

    DeformableSoftBodyBackend::DeformableSoftBodyBackend(std::unique_ptr<IDeformableDriver> driver)
        : _driver(std::move(driver))
    {
    }

    DeformableSoftBodyBackend::~DeformableSoftBodyBackend()
    {
        ReleaseAll();
    }

    DeformableMaterialParams DeformableSoftBodyBackend::MakeMaterial(const Physics::SoftBodyParams &params)
    {
        const Physics::SoftBodyParams defaults;
        DeformableMaterialParams material;
        material.youngsModulus = std::clamp(Finite(params.stiffness, defaults.stiffness), kMinStiffness, kMaxStiffness);
        material.poisson = std::clamp(Finite(params.poisson, defaults.poisson), 0.0f, 0.49f);
        material.dynamicFriction = std::clamp(Finite(params.friction, defaults.friction), 0.0f, 1.0f);
        // The component's damping is a velocity decay in 1/s, which is what PhysX's body linear damping is; the
        // material's elasticity damping stays 0 (an internal, stiffness-relative damping with no CPU counterpart)
        material.elasticityDamping = 0.0f;
        material.linearDamping = std::clamp(Finite(params.damping, defaults.damping), 0.0f, 1000.0f);
        const std::uint32_t iterations = std::max(params.solverIterations, 1u);
        material.solverIterations = std::min(iterations * kIterationsPerSolverIteration, kMaxPositionIterations);
        material.materialModel = params.materialModel;
        material.selfCollision = params.selfCollision;
        return material;
    }

    DeformableSoftBodyBackend::Body *DeformableSoftBodyBackend::Find(const Physics::SoftBodyHandle handle)
    {
        // The generation check comes first: a handle of another backend object (a replaced or recreated one, at
        // whatever address) is never looked up
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(Physics::SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    const DeformableSoftBodyBackend::Body *DeformableSoftBodyBackend::Find(const Physics::SoftBodyHandle handle) const
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _bodies.find(Physics::SoftBodyHandleIndex(handle));
        return it == _bodies.end() ? nullptr : &it->second;
    }

    void DeformableSoftBodyBackend::Fail(const std::string &reason)
    {
        if (_failed)
        {
            return;
        }
        _failed = true;
        _failureReason = reason;
        Logger::Warn(std::format("PhysX GPU soft bodies: {}; the GPU backend is switched off", reason));
    }

    bool DeformableSoftBodyBackend::RefreshNodes(Body &body)
    {
        const std::span<const DeformableVec4> collision = body.staging.collisionPositions;
        for (std::size_t i = 0; i < body.nodes.size(); ++i)
        {
            const DeformableVec4 &p = collision[body.nodeMap[i]];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                return false;
            }
        }
        for (std::size_t i = 0; i < body.nodes.size(); ++i)
        {
            body.nodes[i] = collision[body.nodeMap[i]];
        }
        return true;
    }

    Physics::SoftBodyHandle DeformableSoftBodyBackend::CreateBody(const Physics::SoftBodyDesc &desc)
    {
        if (_failed || !desc.tetMesh || !MeshIsUsable(*desc.tetMesh))
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
        const Physics::TetMeshData &mesh = *desc.tetMesh;

        Body body;
        body.mesh = desc.tetMesh;
        std::vector<Math::Vector3> rest;
        OrientTets(mesh, desc.localToWorld, rest, body.tets, body.flipped);
        body.material = MakeMaterial(desc.params);

        const float density = std::clamp(Finite(desc.params.density, Physics::SoftBodyParams{}.density), kMinDensity,
                                         kMaxDensity);
        std::string error;
        body.driverId = _driver->CreateVolume(
            DeformableVolumeDesc{.restPositions = rest, .tets = body.tets, .density = density, .material = body.material},
            error);
        if (body.driverId == kInvalidDeformableVolume)
        {
            Fail(error.empty() ? "the GPU could not create a deformable volume" : error);
            return Physics::kInvalidSoftBody;
        }

        body.staging = _driver->GetStaging(body.driverId);
        body.nodeMap = _driver->GetNodeMap(body.driverId);
        bool consistent = body.nodeMap.size() == rest.size() && body.staging.simVelocities.size() > 0 &&
                          body.staging.simPositions.size() == body.staging.simVelocities.size();
        if (consistent)
        {
            consistent = std::ranges::all_of(body.nodeMap, [&](const std::uint32_t vertex)
                                             { return vertex < body.staging.collisionPositions.size(); });
        }
        if (!consistent)
        {
            _driver->DestroyVolume(body.driverId);
            Fail("the GPU deformable volume does not match the tetrahedral mesh it was made from");
            return Physics::kInvalidSoftBody;
        }

        // The body starts at its rest shape
        body.nodes.resize(rest.size());
        for (std::size_t i = 0; i < rest.size(); ++i)
        {
            body.nodes[i] = {rest[i].x, rest[i].y, rest[i].z, 0.0f};
        }

        const Physics::SoftBodyHandle handle = Physics::MakeSoftBodyHandle(GetGeneration(), _nextIndex++);
        _bodies.emplace(Physics::SoftBodyHandleIndex(handle), std::move(body));
        return handle;
    }

    void DeformableSoftBodyBackend::Destroy(const Physics::SoftBodyHandle handle)
    {
        if (Body *body = Find(handle))
        {
            _driver->DestroyVolume(body->driverId);
            _bodies.erase(Physics::SoftBodyHandleIndex(handle));
        }
    }

    void DeformableSoftBodyBackend::ReleaseAll()
    {
        for (auto &[index, body] : _bodies)
        {
            static_cast<void>(index);
            _driver->DestroyVolume(body.driverId);
        }
        _bodies.clear();
    }

    void DeformableSoftBodyBackend::SetParams(const Physics::SoftBodyHandle handle,
                                              const Physics::SoftBodyParams &params)
    {
        Body *body = Find(handle);
        if (!body || _failed)
        {
            return;
        }
        const DeformableMaterialParams material = MakeMaterial(params);
        if (!(material == body->material))
        {
            _driver->SetMaterial(body->driverId, material);
            body->material = material;
        }
    }

    void DeformableSoftBodyBackend::AddVelocity(const Physics::SoftBodyHandle handle, const Math::Vector3 &velocity)
    {
        Body *body = Find(handle);
        if (!body || _failed || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
        {
            return;
        }
        // Read-modify-write: the simulation's velocities are on the GPU, so they are read back, the push added to
        // every node that has mass, and the velocities sent up again (positions are left alone)
        std::string error;
        if (!_driver->ReadBack(body->driverId, true, error))
        {
            Fail(error.empty() ? "the soft body velocities could not be read back from the GPU" : error);
            return;
        }
        const std::span<DeformableVec4> velocities = body->staging.simVelocities;
        const std::span<const DeformableVec4> positions = body->staging.simPositions;
        for (std::size_t i = 0; i < velocities.size(); ++i)
        {
            if (positions[i].w > 0.0f)
            {
                velocities[i].x += velocity.x;
                velocities[i].y += velocity.y;
                velocities[i].z += velocity.z;
            }
        }
        if (!_driver->Upload(body->driverId, kDeformSimVelocities, error))
        {
            Fail(error.empty() ? "the soft body velocities could not be sent to the GPU" : error);
        }
    }

    void DeformableSoftBodyBackend::OnSceneStepped(const float dt)
    {
        static_cast<void>(dt);
        if (_failed)
        {
            return;
        }
        for (auto &[index, body] : _bodies)
        {
            static_cast<void>(index);
            std::string error;
            if (!_driver->ReadBack(body.driverId, false, error))
            {
                Fail(error.empty() ? "the soft body positions could not be read back from the GPU" : error);
                return;
            }
            // A body that blew up (a non-finite position) keeps its last good shape rather than poisoning the picture
            static_cast<void>(RefreshNodes(body));
        }
    }

    void DeformableSoftBodyBackend::CopySurfacePositions(const Physics::SoftBodyHandle handle,
                                                         std::vector<Math::Vector3> &out) const
    {
        out.clear();
        const Body *body = Find(handle);
        if (!body)
        {
            return;
        }
        // The embedding and skinning code both soft-body backends share
        SkinSurface<DeformableVec4>(*body->mesh, body->tets, body->flipped, body->nodes, out);
    }

    void DeformableSoftBodyBackend::CopyNodePositions(const Physics::SoftBodyHandle handle,
                                                      std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (const Body *body = Find(handle))
        {
            for (const DeformableVec4 &p : body->nodes)
            {
                out.emplace_back(p.x, p.y, p.z);
            }
        }
    }
}
