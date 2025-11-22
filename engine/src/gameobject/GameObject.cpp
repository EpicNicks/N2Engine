#include "engine/GameObject.hpp"
#include "engine/Component.hpp"
#include "engine/Positionable.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

using namespace N2Engine;

GameObject::Ptr GameObject::Create(const std::string &name)
{
    return std::make_shared<GameObject>(name);
}

// GameObject starts active by default
GameObject::GameObject(std::string name) :
    _name(std::move(name)),
    _positionable{nullptr}
{
}

void GameObject::Purge()
{
    // Clean up components
    for (auto &component : _components)
    {
        if (component)
        {
            component->OnDestroy();
        }
    }
    _components.clear();
    _componentMap.clear();

    // Remove from parent
    if (auto parent = _parent.lock())
    {
        parent->RemoveChild(shared_from_this(), false);
    }

    // Clear children (they will handle their own cleanup)
    _children.clear();

    // Clear positionable
    _positionable.reset();
}

bool GameObject::IsActiveInHierarchy() const
{
    if (_isMarkedForDestruction)
    {
        return false;
    }

    if (_activeInHierarchyDirty)
    {
        UpdateActiveInHierarchyCache();
    }

    return _activeInHierarchyCached;
}

void GameObject::UpdateActiveInHierarchyCache() const
{
    if (auto parent = _parent.lock())
    {
        _activeInHierarchyCached = _isActive && parent->IsActiveInHierarchy();
    }
    else
    {
        _activeInHierarchyCached = _isActive;
    }
    _activeInHierarchyDirty = false;
}

void GameObject::SetActive(bool active)
{
    if (_isActive != active)
    {
        _isActive = active;

        // Mark hierarchy as dirty
        _activeInHierarchyDirty = true;
        for (auto &child : _children)
        {
            child->_activeInHierarchyDirty = true;
        }

        NotifyActiveChanged();
    }
}

void GameObject::NotifyActiveChanged() const
{
    // Notify all components
    for (auto &component : _components)
    {
        if (component->_isActive = IsActiveInHierarchy())
        {
            component->OnEnable();
        }
        else
        {
            component->OnDisable();
        }
    }

    // Recursively notify children
    for (auto &child : _children)
    {
        child->NotifyActiveChanged();
    }
}

void GameObject::SetParent(Ptr parent, bool keepWorldPosition)
{
    if (parent.get() == this)
    {
        return;
    }

    auto oldParent = _parent.lock();
    if (oldParent == parent)
    {
        return;
    }

    // Remove from old parent
    if (oldParent)
    {
        oldParent->RemoveChild(shared_from_this(), keepWorldPosition);
    }

    // Add to new parent
    if (parent)
    {
        parent->AddChild(shared_from_this(), keepWorldPosition);
    }
    else
    {
        _parent.reset();
        _activeInHierarchyDirty = true;

        if (_scene)
        {
            _scene->AddRootGameObject(shared_from_this());
        }
    }
}

void GameObject::AddChild(Ptr child, bool keepWorldPosition)
{
    if (!child || child.get() == this)
        return;

    // Remove from old parent
    if (auto oldParent = child->_parent.lock())
    {
        if (oldParent.get() == this)
            return; // Already our child
        oldParent->RemoveChild(child, keepWorldPosition);
    }

    // Handle transform parenting
    if (keepWorldPosition && child->HasPositionable() && HasPositionable())
    {
        // Store world transform before parenting
        auto childPositionable = child->GetPositionable();
        Math::Vector3 worldPos = childPositionable->GetPosition();
        Math::Quaternion worldRot = childPositionable->GetRotation();
        Math::Vector3 worldScale = childPositionable->GetScale();

        // Set up parent-child relationship first
        child->_parent = weak_from_this();
        _children.push_back(child);
        child->_activeInHierarchyDirty = true;

        // Notify positionable of hierarchy change
        childPositionable->OnHierarchyChanged();

        // Restore world transform
        childPositionable->SetPosition(worldPos);
        childPositionable->SetRotation(worldRot);
        childPositionable->SetScale(worldScale);
    }
    else
    {
        // Set up parent-child relationship
        child->_parent = weak_from_this();
        _children.push_back(child);
        child->_activeInHierarchyDirty = true;

        // Notify positionable of hierarchy change
        if (child->HasPositionable())
        {
            child->GetPositionable()->OnHierarchyChanged();
        }
    }

    // Update scene reference
    child->SetScene(_scene);
}

void GameObject::RemoveChild(Ptr child, bool keepWorldPosition)
{
    if (!child)
    {
        return;
    }

    auto it = std::find(_children.begin(), _children.end(), child);
    if (it != _children.end())
    {
        // Handle transform deparenting
        if (keepWorldPosition && child->HasPositionable() && HasPositionable())
        {
            // Store world transform before deparenting
            auto childPositionable = child->GetPositionable();
            Math::Vector3 worldPos = childPositionable->GetPosition();
            Math::Quaternion worldRot = childPositionable->GetRotation();
            Math::Vector3 worldScale = childPositionable->GetScale();

            // Remove parent-child relationship
            child->_parent.reset();
            child->_activeInHierarchyDirty = true;
            _children.erase(it);

            // Notify positionable of hierarchy change
            childPositionable->OnHierarchyChanged();

            // Restore world transform
            childPositionable->SetPosition(worldPos);
            childPositionable->SetRotation(worldRot);
            childPositionable->SetScale(worldScale);
        }
        else
        {
            // Remove parent-child relationship
            child->_parent.reset();
            child->_activeInHierarchyDirty = true;
            _children.erase(it);

            // Notify positionable of hierarchy change
            if (child->HasPositionable())
            {
                child->GetPositionable()->OnHierarchyChanged();
            }
        }
    }
}

GameObject::Ptr GameObject::GetChild(size_t index) const
{
    if (index < _children.size())
    {
        return _children[index];
    }
    return nullptr;
}

GameObject::Ptr GameObject::FindChild(const std::string &name) const
{
    for (const auto &child : _children)
    {
        if (child->GetName() == name)
        {
            return child;
        }
    }
    return nullptr;
}

GameObject::Ptr GameObject::FindChildRecursive(const std::string &name) const
{
    // Check direct children first
    for (const auto &child : _children)
    {
        if (child->GetName() == name)
        {
            return child;
        }
    }

    // Check children's children recursively
    for (const auto &child : _children)
    {
        auto found = child->FindChildRecursive(name);
        if (found)
        {
            return found;
        }
    }

    return nullptr;
}

Positionable* GameObject::GetPositionable() const
{
    return _positionable.get();
}

void GameObject::CreatePositionable()
{
    if (!_positionable)
    {
        _positionable = std::make_unique<Positionable>(*this);
    }
}

bool GameObject::HasPositionable() const
{
    return _positionable != nullptr;
}

Component* GameObject::GetComponent(const std::type_index &type) const
{
    if (const auto it = _componentMap.find(type); it != _componentMap.end())
    {
        return it->second;
    }
    return nullptr;
}

bool GameObject::RemoveComponent(const std::type_index &type)
{
    if (const auto it = _componentMap.find(type); it != _componentMap.end())
    {
        const auto component = it->second;
        // Notify component
        component->OnDestroy();

        // Remove from map
        _componentMap.erase(it);

        // Remove from vector
        if (const auto vecIt = std::ranges::find_if(_components,
            [component](const std::unique_ptr<Component>& ptr) {
                return ptr.get() == component;
            }); vecIt != _components.end())
        {
            _components.erase(vecIt);
        }

        return true;
    }
    return false;
}

void GameObject::RemoveAllComponents()
{
    for (auto &component : _components)
    {
        if (component)
        {
            component->OnDestroy();
        }
    }

    _components.clear();
    _componentMap.clear();
}

size_t GameObject::GetComponentCount() const
{
    return _components.size();
}

void GameObject::SetScene(Scene *scene)
{
    _scene = scene;
    for (const auto &component : _components)
    {
        _scene->AddComponentToAttachQueue(component.get());
    }

    // Recursively set scene for children
    for (const auto &child : _children)
    {
        child->SetScene(scene);
    }
}

void GameObject::Destroy()
{
    _isMarkedForDestruction = true;
    if (_scene != nullptr)
    {
        _scene->DestroyGameObject(shared_from_this());
    }
}

bool GameObject::IsDestroyed() const
{
    return _isMarkedForDestruction;
}

Scheduling::Coroutine *GameObject::StartCoroutine(std::generator<N2Engine::Scheduling::ICoroutineWait> &&coroutine)
{
    return Scheduling::CoroutineScheduler::StartCoroutine(this, std::move(coroutine));
}
bool GameObject::StopCoroutine(Scheduling::Coroutine *coroutine)
{
    return Scheduling::CoroutineScheduler::StopCoroutine(this, coroutine);
}
void GameObject::StopAllCoroutines()
{
    return Scheduling::CoroutineScheduler::StopAllCoroutines(this);
}

// Utility methods
bool GameObject::IsChildOf(const Ptr& potentialParent) const
{
    if (!potentialParent)
        return false;

    auto currentParent = _parent.lock();
    while (currentParent)
    {
        if (currentParent == potentialParent)
            return true;
        currentParent = currentParent->_parent.lock();
    }
    return false;
}

bool GameObject::IsParentOf(const Ptr& potentialChild)
{
    return potentialChild ? potentialChild->IsChildOf(shared_from_this()) : false;
}

std::vector<GameObject::Ptr> GameObject::GetChildrenRecursive() const
{
    std::vector<Ptr> result;

    for (const auto &child : _children)
    {
        result.push_back(child);

        // Get children's children recursively
        auto grandChildren = child->GetChildrenRecursive();
        result.insert(result.end(), grandChildren.begin(), grandChildren.end());
    }

    return result;
}

GameObject::Ptr GameObject::GetRoot()
{
    GameObject::Ptr current = shared_from_this();
    auto parent = current->_parent.lock();

    while (parent)
    {
        current = parent;
        parent = current->_parent.lock();
    }

    return current;
}

size_t GameObject::GetHierarchyDepth() const
{
    size_t depth = 0;
    auto parent = _parent.lock();

    while (parent)
    {
        depth++;
        parent = parent->_parent.lock();
    }

    return depth;
}

std::string GameObject::GetHierarchyPath() const
{
    std::vector<std::string> names;
    auto current = shared_from_this();

    while (current)
    {
        names.push_back(current->GetName());
        current = current->_parent.lock();
    }

    if (names.empty())
        return "";

    // Reverse to get root-to-this order
    std::reverse(names.begin(), names.end());

    std::string path = names[0];
    for (size_t i = 1; i < names.size(); ++i)
    {
        path += "/" + names[i];
    }

    return path;
}

void GameObject::SetActiveRecursive(bool active)
{
    SetActive(active);

    for (auto &child : _children)
    {
        child->SetActiveRecursive(active);
    }
}

// Static utility methods
GameObject::Ptr GameObject::FindGameObjectByName(const std::string &name, Scene *scene)
{
    if (!scene)
        return nullptr;

    // This would require Scene to have a method to get all GameObjects
    // Implementation depends on Scene's internal structure
    return nullptr;
}

std::vector<GameObject::Ptr> GameObject::FindGameObjectsByTag(const std::string &tag, Scene *scene)
{
    // This would require a tag system to be implemented
    // For now, return empty vector
    return {};
}

using json = nlohmann::json;

json GameObject::Serialize() const
{
    json j;

    // Serialize Asset base class (UUID)
    j["uuid"] = GetUUID().ToString();

    // GameObject-specific data
    j["name"] = _name;
    j["isActive"] = _isActive;
    if (_prefabReference.has_value())
    {
        j["prefabReference"] = _prefabReference->ToString();
    }

    // Positionable (optional)
    if (HasPositionable())
    {
        j["positionable"] = GetPositionable()->Serialize();
    }

    // Components
    json components = json::array();
    for (const auto &component : _components)
    {
        json compJson;
        compJson["type"] = component->GetTypeName();
        compJson["data"] = component->Serialize();
        components.push_back(compJson);
    }
    j["components"] = components;

    // Children (recursive)
    json children = json::array();
    for (const auto &child : _children)
    {
        children.push_back(child->Serialize());
    }
    j["children"] = children;

    return j;
}

GameObject::Ptr GameObject::Deserialize(const json &j, ReferenceResolver *resolver)
{
    // Create GameObject with UUID
    Math::UUID uuid(j["uuid"].get<std::string>());
    auto go = std::make_shared<GameObject>(j["name"].get<std::string>());
    go->_uuid = uuid; // Restore original UUID

    // Register this GameObject in the resolver
    if (resolver)
    {
        resolver->RegisterGameObject(uuid, go.get());
    }

    if (j.contains("isActive"))
    {
        go->_isActive = j["isActive"];
    }

    if (j.contains("prefabReference"))
    {
        go->_prefabReference = Math::UUID(j["prefabReference"].get<std::string>());
    }

    // Deserialize Positionable
    if (j.contains("positionable"))
    {
        go->CreatePositionable();
        go->_positionable->Deserialize(j["positionable"]);
    }

    // Deserialize Components
    if (j.contains("components"))
    {
        for (const auto &compJson : j["components"])
        {
            std::string typeName = compJson["type"];

            if (auto component = ComponentRegistry::Instance().Create(typeName, *go))
            {
                // Register component in resolver BEFORE deserializing
                if (resolver && compJson["data"].contains("uuid"))
                {
                    Math::UUID compUUID(compJson["data"]["uuid"].get<std::string>());
                    resolver->RegisterComponent(compUUID, component.get());
                }

                // Deserialize with resolver for reference resolution
                component->Deserialize(compJson["data"], resolver);

                go->_components.push_back(std::move(component));
                go->_componentMap[std::type_index(typeid(*component))] = component.get();
            }
        }
    }

    // Deserialize Children (recursive)
    if (j.contains("children"))
    {
        for (const auto &childJson : j["children"])
        {
            const auto child = Deserialize(childJson, resolver);
            go->AddChild(child, false); // Don't keep world position during load
        }
    }

    return go;
}