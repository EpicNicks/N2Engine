#include "engine/Positionable.hpp"
#include "engine/GameObject.hpp"
#include "engine/serialization/MathSerialization.hpp"

using namespace N2Engine;
using namespace N2Engine::Math;

Positionable::Positionable(GameObject &gameObject)
    : _gameObject(gameObject)
{
    // Initialize with identity transform
    _localTransform = Transform::Identity();
}

void Positionable::SetLocalPosition(const Math::Vector3 &position)
{
    if (_localTransform.GetPosition() != position)
    {
        _localTransform.SetPosition(position);
        MarkGlobalTransformDirty();
    }
}

void Positionable::SetLocalRotation(const Math::Quaternion &rotation)
{
    if (_localTransform.GetRotation() != rotation)
    {
        _localTransform.SetRotation(rotation);
        MarkGlobalTransformDirty();
    }
}

void Positionable::SetLocalScale(const Math::Vector3 &scale)
{
    if (_localTransform.GetScale() != scale)
    {
        _localTransform.SetScale(scale);
        MarkGlobalTransformDirty();
    }
}

Vector3 Positionable::GetPosition() const
{
    if (_globalTransformDirty)
    {
        UpdateGlobalTransform();
    }
    return _cachedGlobalTransform.GetPosition();
}

Quaternion Positionable::GetRotation() const
{
    if (_globalTransformDirty)
    {
        UpdateGlobalTransform();
    }
    return _cachedGlobalTransform.GetRotation();
}

Vector3 Positionable::GetScale() const
{
    if (_globalTransformDirty)
    {
        UpdateGlobalTransform();
    }
    return _cachedGlobalTransform.GetScale();
}

void Positionable::SetPosition(const Math::Vector3 &position)
{
    auto parentPositionable = GetParentPositionable();
    if (parentPositionable)
    {
        // Convert world position to local space
        Math::Vector3 localPos = parentPositionable->InverseTransformPoint(position);
        SetLocalPosition(localPos);
    }
    else
    {
        SetLocalPosition(position);
    }
}

void Positionable::SetRotation(const Math::Quaternion &rotation)
{
    auto parentPositionable = GetParentPositionable();
    if (parentPositionable)
    {
        // Convert world rotation to local space
        Math::Quaternion parentRotation = parentPositionable->GetRotation();
        Math::Quaternion localRot = parentRotation.Inverse() * rotation;
        SetLocalRotation(localRot);
    }
    else
    {
        SetLocalRotation(rotation);
    }
}

void Positionable::SetScale(const Math::Vector3 &scale)
{
    auto parentPositionable = GetParentPositionable();
    if (parentPositionable)
    {
        // Convert world scale to local space
        Math::Vector3 parentScale = parentPositionable->GetScale();
        Math::Vector3 localScale = Math::Vector3{
            scale.x / parentScale.x,
            scale.y / parentScale.y,
            scale.z / parentScale.z};
        SetLocalScale(localScale);
    }
    else
    {
        SetLocalScale(scale);
    }
}

void Positionable::SetLocalPositionAndRotation(const Math::Vector3 &position, const Math::Quaternion &rotation)
{
    bool changed = false;
    if (_localTransform.GetPosition() != position)
    {
        _localTransform.SetPosition(position);
        changed = true;
    }
    if (_localTransform.GetRotation() != rotation)
    {
        _localTransform.SetRotation(rotation);
        changed = true;
    }

    if (changed)
    {
        MarkGlobalTransformDirty();
    }
}

void Positionable::SetPositionAndRotation(const Math::Vector3 &position, const Math::Quaternion &rotation)
{
    auto parentPositionable = GetParentPositionable();
    if (parentPositionable)
    {
        Math::Vector3 localPos = parentPositionable->InverseTransformPoint(position);
        Math::Quaternion parentRot = parentPositionable->GetRotation();
        Math::Quaternion localRot = parentRot.Inverse() * rotation;
        SetLocalPositionAndRotation(localPos, localRot);
    }
    else
    {
        SetLocalPositionAndRotation(position, rotation);
    }
}

void Positionable::SetLocalTransform(const Transform &transform)
{
    if (_localTransform != transform)
    {
        _localTransform = transform;
        MarkGlobalTransformDirty();
    }
}

const Transform &Positionable::GetLocalTransform() const
{
    return _localTransform;
}

const Transform &Positionable::GetGlobalTransform() const
{
    if (_globalTransformDirty)
    {
        UpdateGlobalTransform();
    }
    return _cachedGlobalTransform;
}

void Positionable::MarkGlobalTransformDirty() const
{
    if (!_globalTransformDirty)
    {
        _globalTransformDirty = true;
        _hierarchyVersion++;
        MarkChildrenGlobalTransformDirty();
    }
}

void Positionable::MarkChildrenGlobalTransformDirty() const
{
    for (const auto &child : _gameObject.GetChildren())
    {
        if (auto childPositionable = child->GetPositionable())
        {
            childPositionable->MarkGlobalTransformDirty();
        }
    }
}

void Positionable::UpdateGlobalTransform() const
{
    _cachedGlobalTransform = CalculateGlobalTransform();
    _globalTransformDirty = false;
    _globalTransformValid = true;
    _cachedHierarchyVersion = _hierarchyVersion;
}

Transform Positionable::CalculateGlobalTransform() const
{
    auto parentPositionable = GetParentPositionable();
    if (parentPositionable)
    {
        // Combine parent's global transform with our local transform
        const Transform &parentGlobal = parentPositionable->GetGlobalTransform();
        return Transform::Combine(parentGlobal, _localTransform);
    }
    else
    {
        // No parent, so local transform IS the global transform
        return _localTransform;
    }
}

std::shared_ptr<Positionable> Positionable::GetParentPositionable() const
{
    _gameObject;
    if (auto parent = _gameObject.GetParent())
    {
        return parent->GetPositionable();
    }
    return nullptr;
}

Positionable::Matrix4 Positionable::GetLocalToWorldMatrix() const
{
    return GetGlobalTransform().GetMatrix();
}

Positionable::Matrix4 Positionable::GetWorldToLocalMatrix() const
{
    return GetLocalToWorldMatrix().inverse();
}

Vector3 Positionable::TransformPoint(const Math::Vector3 &point) const
{
    Matrix4 matrix = GetLocalToWorldMatrix();
    return matrix.TransformPoint(point);
}

Vector3 Positionable::TransformDirection(const Math::Vector3 &direction) const
{
    const Transform &global = GetGlobalTransform();
    return global.GetRotation() * direction;
}

Vector3 Positionable::InverseTransformPoint(const Math::Vector3 &point) const
{
    Matrix4 matrix = GetWorldToLocalMatrix();
    return matrix.TransformPoint(point);
}

Vector3 Positionable::InverseTransformDirection(const Math::Vector3 &direction) const
{
    const Transform &global = GetGlobalTransform();
    return global.GetRotation().Inverse() * direction;
}

void Positionable::OnHierarchyChanged()
{
    MarkGlobalTransformDirty();
}

bool Positionable::IsGlobalTransformDirty() const
{
    return _globalTransformDirty;
}

using json = nlohmann::json;

json Positionable::Serialize() const
{
    json j;

    // Much simpler now - nlohmann::json knows how to serialize these types
    j["localPosition"] = GetLocalPosition();
    j["localRotation"] = GetLocalRotation();
    j["localScale"] = GetLocalScale();

    return j;
}

void Positionable::Deserialize(const json &j)
{
    if (j.contains("localPosition"))
    {
        SetLocalPosition(j["localPosition"].get<Math::Vector3>());
    }

    if (j.contains("localRotation"))
    {
        SetLocalRotation(j["localRotation"].get<Math::Quaternion>());
    }

    if (j.contains("localScale"))
    {
        SetLocalScale(j["localScale"].get<Math::Vector3>());
    }
}