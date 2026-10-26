#pragma once

#include <string_view>
#include <vector>

#include "engine/physics/DeterminismLevel.hpp"
#include "engine/physics/PhysicsBackendKind.hpp"
#include "engine/physics/PhysicsFeatures.hpp"

namespace N2Engine::Physics
{
    /// The backend half of the Deterministic preset: the backend declares CrossPlatform determinism and implements every
    /// feature group (the physics-backend and physics-features requirements of ValidateNetwork). The other requirements
    /// (strict floating point, scalar SIMD, the Lua seed and so on) are project and build settings, not a backend's
    /// declaration, and are still checked at startup.
    [[nodiscard]] constexpr bool DeterministicSelectable(const DeterminismLevel level, const PhysicsFeatureSet features)
    {
        return level == DeterminismLevel::CrossPlatform && features.IsComplete();
    }

    /// What one physics backend declares, read without creating it (the editor's backend picker, protocol 1.20.0). The
    /// values are the ones Application::Init gives the Deterministic preset's startup checks: the backend class's own
    /// static declarations. For a backend this build does not contain (compiledIn false) there is no class to ask, so
    /// the declarations are the empty ones (None, no groups, no snapshot or set-state) and mean "unknown", not "none".
    struct PhysicsBackendDescription
    {
        PhysicsBackendKind kind = PhysicsBackendKind::PhysX;
        /// The backend is in this build (IsBackendCompiledIn)
        bool compiledIn = false;
        /// DeclaredDeterminism(true): the level with physics.deterministic on, the best the backend can declare in this build
        DeterminismLevel determinism = DeterminismLevel::None;
        /// DeclaredDeterminism(false): the level with physics.deterministic off (the default)
        DeterminismLevel determinismDefault = DeterminismLevel::None;
        /// The feature groups it implements (kImplementedFeatures)
        PhysicsFeatureSet features;
        bool supportsSnapshot = false;
        bool supportsSetState = false;

        /// Whether the Deterministic network preset can start on this backend as far as the backend's own declarations
        /// go (DeterministicSelectable)
        [[nodiscard]] bool DeterministicSelectable() const
        {
            return compiledIn && Physics::DeterministicSelectable(determinism, features);
        }
    };

    /// A short stable id for a feature group, for the protocol: "layers", "triggers", "events" or "queries"
    [[nodiscard]] constexpr std::string_view FeatureId(const PhysicsFeature feature)
    {
        switch (feature)
        {
        case PhysicsFeature::Layers:
            return "layers";
        case PhysicsFeature::Triggers:
            return "triggers";
        case PhysicsFeature::Events:
            return "events";
        case PhysicsFeature::Queries:
            return "queries";
        }
        return "unknown";
    }

    /// Both known backends, PhysX first, each as this build declares it
    [[nodiscard]] std::vector<PhysicsBackendDescription> DescribePhysicsBackends();

    /// One backend as this build declares it
    [[nodiscard]] PhysicsBackendDescription DescribePhysicsBackend(PhysicsBackendKind kind);
}
