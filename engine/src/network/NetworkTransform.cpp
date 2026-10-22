#include "engine/network/NetworkTransform.hpp"

#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"

namespace N2Engine::Network
{
    namespace
    {
        /// The object's Positionable, made if it has none yet
        Positionable *PositionableOf(GameObject &object)
        {
            if (!object.HasPositionable())
            {
                object.CreatePositionable();
            }
            return object.GetPositionable();
        }
    }

    NetworkTransform::NetworkTransform(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("positionStep", _positionStep)
            .Range(0.0001, 100.0)
            .Tooltip("The position is sent as whole steps of this many units per axis: its error is at most half of it");
        RegisterMember("replicatePosition", _replicatePosition);
        RegisterMember("replicateRotation", _replicateRotation);
        RegisterMember("replicateScale", _replicateScale);
        // The replication layer's cache of the transform: what the server reads and a client sets
        RegisterMember("position", _position).Replicated(0.01).Hidden().ReadOnly();
        RegisterMember("rotation", _rotation).Replicated().Hidden().ReadOnly();
        RegisterMember("scale", _scale).Replicated().Hidden().ReadOnly();
        RegisterMember("teleportSerial", _teleportSerial).Replicated().Hidden().ReadOnly();
    }

    std::vector<FieldInfo> NetworkTransform::DescribeFields() const
    {
        std::vector<FieldInfo> fields = SerializableComponent::DescribeFields();
        for (FieldInfo &field : fields)
        {
            if (field.name == "position")
            {
                field.replicated = _replicatePosition;
                field.replicationStep = static_cast<double>(_positionStep);
            }
            else if (field.name == "rotation")
            {
                field.replicated = _replicateRotation;
            }
            else if (field.name == "scale")
            {
                field.replicated = _replicateScale;
            }
        }
        return fields;
    }

    nlohmann::json NetworkTransform::SerializeReplicated(const std::span<const FieldInfo> /*replicated*/)
    {
        if (const Positionable *const positionable = GetGameObject().GetPositionable())
        {
            _position = positionable->GetLocalPosition();
            _rotation = positionable->GetLocalRotation();
            _scale = positionable->GetLocalScale();
        }
        return Serialize();
    }

    void NetworkTransform::ApplyReplicated(const nlohmann::json &values, ReferenceResolver *resolver)
    {
        SetEditorFields(values, resolver);
        if (!values.is_object())
        {
            return;
        }
        Positionable *const positionable = PositionableOf(GetGameObject());
        if (positionable != nullptr)
        {
            if (values.contains("position"))
            {
                positionable->SetLocalPosition(_position);
            }
            if (values.contains("rotation"))
            {
                positionable->SetLocalRotation(_rotation);
            }
            if (values.contains("scale"))
            {
                positionable->SetLocalScale(_scale);
            }
        }
        if (values.contains("teleportSerial") && _teleportSerial != _seenTeleportSerial)
        {
            _seenTeleportSerial = _teleportSerial;
            _teleportPending = true;
        }
    }

    void NetworkTransform::Teleport(const Math::Vector3 &position)
    {
        if (Positionable *const positionable = PositionableOf(GetGameObject()))
        {
            positionable->SetLocalPosition(position);
        }
        ++_teleportSerial;
    }

    void NetworkTransform::Teleport(const Math::Vector3 &position, const Math::Quaternion &rotation)
    {
        if (Positionable *const positionable = PositionableOf(GetGameObject()))
        {
            positionable->SetLocalPositionAndRotation(position, rotation);
        }
        ++_teleportSerial;
    }
}
