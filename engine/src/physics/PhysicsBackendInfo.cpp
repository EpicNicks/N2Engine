#include "engine/physics/PhysicsBackendInfo.hpp"

#include "engine/physics/jolt/JoltBackend.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"

namespace N2Engine::Physics
{
    PhysicsBackendDescription DescribePhysicsBackend(const PhysicsBackendKind kind)
    {
        PhysicsBackendDescription description;
        description.kind = kind;
        description.compiledIn = IsBackendCompiledIn(kind);
        switch (kind)
        {
        case PhysicsBackendKind::PhysX:
#ifdef N2ENGINE_PHYSX_ENABLED
            description.determinism = PhysXBackend::DeclaredDeterminism(true);
            description.determinismDefault = PhysXBackend::DeclaredDeterminism(false);
            description.features = PhysXBackend::kImplementedFeatures;
            description.supportsSnapshot = PhysXBackend::kSupportsSnapshot;
            description.supportsSetState = PhysXBackend::kSupportsSetState;
#endif
            break;
        case PhysicsBackendKind::Jolt:
#ifdef N2ENGINE_JOLT_ENABLED
            description.determinism = JoltBackend::DeclaredDeterminism(true);
            description.determinismDefault = JoltBackend::DeclaredDeterminism(false);
            description.features = JoltBackend::kImplementedFeatures;
            description.supportsSnapshot = JoltBackend::kSupportsSnapshot;
            description.supportsSetState = JoltBackend::kSupportsSetState;
#endif
            break;
        }
        return description;
    }

    std::vector<PhysicsBackendDescription> DescribePhysicsBackends()
    {
        return {DescribePhysicsBackend(PhysicsBackendKind::PhysX), DescribePhysicsBackend(PhysicsBackendKind::Jolt)};
    }
}
