#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Physics
{
    /// A group of physics features a backend may or may not implement yet (IPhysicsBackend::GetImplementedFeatures).
    /// The Deterministic network preset needs every group: a lockstep or rollback game whose layers, triggers, events or
    /// queries silently do nothing would diverge from what its author tested.
    enum class PhysicsFeature : std::uint8_t
    {
        Layers = 1u << 0,   // collider layers and the collision matrix (SetColliderLayer, RefreshCollisionMatrix)
        Triggers = 1u << 1, // trigger colliders (SetIsTrigger)
        Events = 1u << 2,   // contact and trigger events (ProcessCollisionCallbacks: Enter, Stay, Exit)
        Queries = 1u << 3   // scene queries (Raycast, RaycastAll, SphereCast, CapsuleCast, Overlap)
    };

    /// Every group, in the order the messages list them
    inline constexpr std::array<PhysicsFeature, 4> kAllPhysicsFeatures = {
        PhysicsFeature::Layers, PhysicsFeature::Triggers, PhysicsFeature::Events, PhysicsFeature::Queries};

    /// How a message names a group
    [[nodiscard]] constexpr std::string_view ToString(const PhysicsFeature feature)
    {
        switch (feature)
        {
        case PhysicsFeature::Layers:
            return "layers and the collision matrix";
        case PhysicsFeature::Triggers:
            return "triggers";
        case PhysicsFeature::Events:
            return "contact and trigger events";
        case PhysicsFeature::Queries:
            return "scene queries";
        }
        return "unknown";
    }

    /// A set of PhysicsFeature groups
    class PhysicsFeatureSet
    {
    public:
        constexpr PhysicsFeatureSet() = default;

        /// No group (what a backend declares unless it says more)
        [[nodiscard]] static constexpr PhysicsFeatureSet None() { return {}; }
        /// Every group
        [[nodiscard]] static constexpr PhysicsFeatureSet All()
        {
            PhysicsFeatureSet set;
            for (const PhysicsFeature feature : kAllPhysicsFeatures)
            {
                set = set.With(feature);
            }
            return set;
        }

        /// This set and `feature`
        [[nodiscard]] constexpr PhysicsFeatureSet With(const PhysicsFeature feature) const
        {
            PhysicsFeatureSet set;
            set._bits = static_cast<std::uint8_t>(_bits | static_cast<std::uint8_t>(feature));
            return set;
        }
        [[nodiscard]] constexpr bool Has(const PhysicsFeature feature) const
        {
            return (_bits & static_cast<std::uint8_t>(feature)) != 0u;
        }
        /// Every group is in the set
        [[nodiscard]] constexpr bool IsComplete() const { return _bits == All()._bits; }
        /// The groups that are not in the set, in kAllPhysicsFeatures order
        [[nodiscard]] std::vector<PhysicsFeature> Missing() const
        {
            std::vector<PhysicsFeature> missing;
            for (const PhysicsFeature feature : kAllPhysicsFeatures)
            {
                if (!Has(feature))
                {
                    missing.push_back(feature);
                }
            }
            return missing;
        }
        /// The missing groups as one list for a message: "triggers, scene queries"; empty when complete
        [[nodiscard]] std::string MissingText() const
        {
            std::string text;
            for (const PhysicsFeature feature : Missing())
            {
                if (!text.empty())
                {
                    text += ", ";
                }
                text += ToString(feature);
            }
            return text;
        }

        [[nodiscard]] constexpr bool operator==(const PhysicsFeatureSet &) const = default;

    private:
        std::uint8_t _bits = 0u;
    };
}
