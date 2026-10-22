#pragma once

#include <span>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Network
{
    /**
     * Replicates the object's local position, rotation and scale from the server to every client (netcode stage B2). On
     * the server it reads the object's transform at every send; on a client it writes what the server sent straight to
     * the transform (no smoothing: interpolation of remote objects is a later stage, which reads GetTeleportSerial and
     * ConsumeTeleport to know when to snap instead). Only the server's value ever counts: a client that moves its copy
     * has it overwritten by the next update that carries a change, and nothing a client sends is read as a transform.
     *
     * The values travel as replicated fields (see FieldInfo::replicated), so they are delta-compressed against what each
     * client acknowledged, like any other replicated field:
     *   position   a whole number of positionStep per axis (default 1 cm), so a position within half a step of what the
     *              client has costs nothing and the error is at most half a step per axis
     *   rotation   smallest-three, 32 bits; the error is under 0.005 radians
     *   scale      full floats (it rarely changes)
     * and `teleportSerial`, which Teleport raises, so a stage that smooths movement can tell a jump from a move.
     * replicatePosition, replicateRotation and replicateScale choose what is replicated (scale: off by default). The
     * members position, rotation, scale and teleportSerial are the replication layer's cache of the transform, hidden and
     * read-only: set the transform, not them.
     *
     * At most one per object, on an object that has a NetworkObject. A server and its clients must agree on the settings
     * (they load the same scene file; a mismatch makes the clients skip this component's entries, never misread them).
     */
    class NetworkTransform final : public SerializableComponent
    {
    public:
        explicit NetworkTransform(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "NetworkTransform"; }

        static constexpr bool IsSingleton = true;

        /// The members the settings name, with replicated set from them and the step of the position
        [[nodiscard]] std::vector<FieldInfo> DescribeFields() const override;

        /// Refreshes the cached transform from the object's, then serialises (what the server reads before encoding)
        [[nodiscard]] nlohmann::json SerializeReplicated(std::span<const FieldInfo> replicated) override;

        /// Sets the cached members from `values`, then writes the transform parts `values` names
        void ApplyReplicated(const nlohmann::json &values, ReferenceResolver *resolver) override;

        /// Moves the object to `position` and tells clients it was a jump, not a move (raises the teleport serial)
        void Teleport(const Math::Vector3 &position);
        void Teleport(const Math::Vector3 &position, const Math::Quaternion &rotation);

        /// Counts the teleports (on a client: the server's, as received)
        [[nodiscard]] int GetTeleportSerial() const noexcept { return _teleportSerial; }
        /// True once after a client received a teleport; clears the flag
        [[nodiscard]] bool ConsumeTeleport() noexcept
        {
            const bool pending = _teleportPending;
            _teleportPending = false;
            return pending;
        }

        [[nodiscard]] float GetPositionStep() const noexcept { return _positionStep; }
        [[nodiscard]] bool ReplicatesPosition() const noexcept { return _replicatePosition; }
        [[nodiscard]] bool ReplicatesRotation() const noexcept { return _replicateRotation; }
        [[nodiscard]] bool ReplicatesScale() const noexcept { return _replicateScale; }

    private:
        float _positionStep = 0.01f;
        bool _replicatePosition = true;
        bool _replicateRotation = true;
        bool _replicateScale = false;

        Math::Vector3 _position{0.0f, 0.0f, 0.0f};
        Math::Quaternion _rotation = Math::Quaternion::Identity;
        Math::Vector3 _scale{1.0f, 1.0f, 1.0f};
        int _teleportSerial = 0;

        int _seenTeleportSerial = 0;
        bool _teleportPending = false;
    };
}
