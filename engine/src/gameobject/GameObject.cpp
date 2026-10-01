#include <algorithm>
#include <format>
#include <typeindex>
#include <typeinfo>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/GameObject.hpp"
#include "engine/Component.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
// ReSharper disable once CppUnusedIncludeDirective
#include "engine/serialization/MathSerialization.hpp"

using namespace N2Engine;

GameObject::Ptr GameObject::Create(const std::string &name)
{
    return std::make_shared<GameObject>(name);
}

// GameObject starts active by default
GameObject::GameObject()
    : _name{"GameObject"},
      _positionable{nullptr} {}

GameObject::GameObject(std::string name)
    : _name(std::move(name)),
      _positionable{nullptr} {}

GameObject::~GameObject()
{
    // A scene that's destroyed first clears this pointer (Scene::~Scene), so it's always alive here
    if (_scene)
    {
        for (const auto &component : _components)
        {
            _scene->DetachComponent(component.get());
        }
        _scene->GetCoroutineScheduler()->StopAllCoroutines(this);
    }
}

void GameObject::Purge()
{
    // OnDestroy already ran in Scene::CallOnDestroyForGameObject; this only releases
    _isTornDown = true;
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

bool GameObject::IsActiveInHierarchyIgnoringDestruction() const
{
    if (!_isActive)
    {
        return false;
    }
    const auto parent = _parent.lock();
    return !parent || parent->IsActiveInHierarchyIgnoringDestruction();
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
    if (_isActive == active)
    {
        return;
    }

    const bool wasActiveInHierarchy = IsActiveInHierarchy();
    _isActive = active;
    MarkActiveInHierarchyDirty();
    NotifyIfActiveChanged(wasActiveInHierarchy);
}

void GameObject::MarkActiveInHierarchyDirty() const
{
    // Every descendant caches its own answer, so all of them must recompute, not just direct children
    _activeInHierarchyDirty = true;
    for (const auto &child : _children)
    {
        child->MarkActiveInHierarchyDirty();
    }
}

void GameObject::NotifyIfActiveChanged(const bool wasActiveInHierarchy) const
{
    if (const bool nowActive = IsActiveInHierarchy(); nowActive != wasActiveInHierarchy)
    {
        NotifyActiveChanged(nowActive);
    }
}

void GameObject::NotifyActiveChanged(const bool nowActive) const
{
    // A component's own _isActive is its enable flag; hierarchy state is IsActiveInHierarchy().
    // Only components that are enabled themselves see their effective state change.
    // Snapshots: a callback can add, remove or reparent, which invalidated the live iteration.
    for (Component *component : SnapshotComponents())
    {
        if (OwnsComponent(component) && component->_isActive && !component->_isMarkedForDestruction &&
            !component->_isRunningDestroyCallbacks)
        {
            nowActive ? component->OnEnable() : component->OnDisable();
        }
    }

    // Children that are inactive themselves stay inactive either way (skipping any a callback moved away)
    const std::vector<Ptr> children = _children;
    for (const auto &child : children)
    {
        if (child->_isActive && child->_parent.lock().get() == this)
        {
            child->NotifyActiveChanged(nowActive);
        }
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

    // Refused before leaving the old parent: AddChild refuses it too, which used to leave this object
    // detached from its parent and not a root
    if (parent && parent->IsChildOf(shared_from_this()))
    {
        Logger::Warn(std::format("Can't make '{}' a child of its descendant '{}'", _name, parent->GetName()));
        return;
    }

    // Remove from old parent
    if (oldParent)
    {
        oldParent->DetachChild(shared_from_this(), keepWorldPosition);
    }

    // Add to new parent
    if (parent)
    {
        parent->AddChild(shared_from_this(), keepWorldPosition);
    }
    else
    {
        _parent.reset();
        MarkActiveInHierarchyDirty();

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

    // Parenting an ancestor under its own descendant would make a cycle
    if (IsChildOf(child))
    {
        Logger::Warn(std::format("Can't make '{}' a child of its descendant '{}'", child->GetName(), _name));
        return;
    }

    // A root object stops being a root: it used to stay in the scene's roots as well, so it was
    // updated, rendered and serialized twice and survived its parent's destruction.
    // Note: this edits the root list directly (as RemoveRootGameObject does), so reparenting a root from
    // inside a TraverseAll/TraverseUntil callback isn't supported.
    if (!child->_parent.lock() && child->_scene)
    {
        std::erase(child->_scene->_rootGameObjects, child);
    }

    // Remove from old parent
    if (auto oldParent = child->_parent.lock())
    {
        if (oldParent.get() == this)
            return; // Already our child
        oldParent->DetachChild(child, keepWorldPosition);
    }

    const bool childWasActive = child->IsActiveInHierarchy();

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
        child->MarkActiveInHierarchyDirty();

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
        child->MarkActiveInHierarchyDirty();

        // Notify positionable of hierarchy change
        if (child->HasPositionable())
        {
            child->GetPositionable()->OnHierarchyChanged();
        }
    }

    // Update scene reference
    child->SetScene(_scene);

    // Parenting under an inactive object deactivates the child's subtree (and vice versa)
    child->NotifyIfActiveChanged(childWasActive);
}

void GameObject::RemoveChild(Ptr child, bool keepWorldPosition)
{
    if (!child || child->_parent.lock().get() != this)
    {
        return;
    }

    // Destroy paths (Purge, Scene's purge) get here too; a destroyed child goes with its hierarchy
    if (child->_isMarkedForDestruction)
    {
        DetachChild(child, keepWorldPosition);
        return;
    }

    // Like Unity's SetParent(null). It used to stay in the scene without being under a root, so it kept
    // updating, Scene::Clear (which walks the roots) never tore it down, and after a scene switch its
    // scene pointer dangled. Its components stay attached: the scene doesn't change, so no second OnAttach.
    child->SetParent(nullptr, keepWorldPosition);
}

void GameObject::DetachChild(Ptr child, bool keepWorldPosition)
{
    if (!child)
    {
        return;
    }

    auto it = std::find(_children.begin(), _children.end(), child);
    if (it != _children.end())
    {
        const bool childWasActive = child->IsActiveInHierarchy();

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
            child->MarkActiveInHierarchyDirty();
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
            child->MarkActiveInHierarchyDirty();
            _children.erase(it);

            // Notify positionable of hierarchy change
            if (child->HasPositionable())
            {
                child->GetPositionable()->OnHierarchyChanged();
            }
        }

        child->NotifyIfActiveChanged(childWasActive);
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
    const auto it = _componentMap.find(type);
    return it != _componentMap.end() && RemoveComponent(it->second);
}

bool GameObject::RemoveComponent(Component *component)
{
    const auto vecIt = std::ranges::find_if(_components, [component](const std::unique_ptr<Component> &ptr)
    {
        return ptr.get() == component;
    });
    if (!component || vecIt == _components.end())
    {
        return false;
    }
    // Removed from its own OnDisable/OnDestroy (or one run by a destroy in progress): whoever started that
    // teardown finishes it once the callbacks return. Freeing it here freed it under that caller.
    if (component->_isRunningDestroyCallbacks)
    {
        return false;
    }

    component->RunDestroyCallbacks(IsActiveInHierarchyIgnoringDestruction());
    const std::type_index componentType = typeid(*component);

    // The scene keeps raw pointers to attached components; drop them before the component is freed
    if (_scene)
    {
        _scene->DetachComponent(component);
    }

    // The map points at the first of each type; only its removal changes what GetComponent finds
    const auto mapIt = _componentMap.find(componentType);
    const bool wasMapped = mapIt != _componentMap.end() && mapIt->second == component;
    if (wasMapped)
    {
        _componentMap.erase(mapIt);
    }

    // Found again: a destroy callback may have changed the vector
    std::erase_if(_components, [component](const std::unique_ptr<Component> &ptr) { return ptr.get() == component; });

    if (wasMapped)
    {
        for (const auto &remaining : _components)
        {
            if (std::type_index(typeid(*remaining)) == componentType)
            {
                _componentMap.emplace(componentType, remaining.get());
                break;
            }
        }
    }

    return true;
}

void GameObject::RemoveAllComponents()
{
    const bool wasActive = IsActiveInHierarchyIgnoringDestruction();
    // A snapshot: the callbacks can add or remove components, which invalidated the live iteration
    for (Component *component : SnapshotComponents())
    {
        if (OwnsComponent(component))
        {
            component->RunDestroyCallbacks(wasActive);
        }
    }

    // Everything goes, including components a callback added, except any whose own callbacks are still
    // running (this was called from one of them): their teardown frees them once it returns
    std::vector<std::unique_ptr<Component>> released;
    for (auto &component : _components)
    {
        if (!component->_isRunningDestroyCallbacks)
        {
            if (_scene)
            {
                _scene->DetachComponent(component.get());
            }
            released.push_back(std::move(component));
        }
    }
    std::erase(_components, nullptr);

    _componentMap.clear();
    for (const auto &remaining : _components)
    {
        _componentMap.emplace(std::type_index(typeid(*remaining)), remaining.get());
    }
}

size_t GameObject::GetComponentCount() const
{
    return _components.size();
}

std::vector<Component *> GameObject::SnapshotComponents() const
{
    std::vector<Component *> snapshot;
    snapshot.reserve(_components.size());
    for (const auto &component : _components)
    {
        snapshot.push_back(component.get());
    }
    return snapshot;
}

bool GameObject::OwnsComponent(const Component *component) const
{
    return std::ranges::any_of(_components, [component](const std::unique_ptr<Component> &owned)
    {
        return owned.get() == component;
    });
}

void GameObject::SetScene(Scene *scene)
{
    if (_scene != scene)
    {
        // Leaving a scene: it must forget these components before they're freed or re-homed, and its
        // scheduler must drop this object's coroutines (it keys them by GameObject*)
        if (_scene)
        {
            for (const auto &component : _components)
            {
                _scene->DetachComponent(component.get());
            }
            _scene->GetCoroutineScheduler()->StopAllCoroutines(this);
        }

        _scene = scene;

        // Joining a scene: attach on its next ProcessAttachQueue. An unchanged scene (e.g. reparenting
        // within it) doesn't re-queue, so components aren't attached twice.
        if (_scene)
        {
            for (const auto &component : _components)
            {
                _scene->AddComponentToAttachQueue(component.get());
            }
        }
    }

    // Recursively set scene for children
    for (const auto &child : _children)
    {
        child->SetScene(scene);
    }
}

void GameObject::Destroy()
{
    if (_scene != nullptr)
    {
        // Don't set _isMarkedForDestruction here: ProcessDestroyed treats a marked object as already
        // handled and skips it, so marking first meant Destroy() never destroyed anything
        _scene->DestroyGameObject(shared_from_this());
    }
    else
    {
        // Not in a scene, so its components were never attached; just flag it as gone
        _isMarkedForDestruction = true;
        _isTornDown = true;
    }
}

bool GameObject::IsDestroyed() const
{
    return _isMarkedForDestruction;
}

// Coroutines run on the object's own scene, which may not be the loaded one (or may not exist)
Scheduling::Coroutine* GameObject::StartCoroutine(std::generator<N2Engine::Scheduling::ICoroutineWait> &&coroutine)
{
    if (!_scene)
    {
        Logger::Warn(std::format("StartCoroutine on '{}', which isn't in a scene", _name));
        return nullptr;
    }
    return _scene->GetCoroutineScheduler()->StartCoroutine(this, std::move(coroutine));
}

bool GameObject::StopCoroutine(Scheduling::Coroutine *coroutine)
{
    return _scene && _scene->GetCoroutineScheduler()->StopCoroutine(this, coroutine);
}

void GameObject::StopAllCoroutines()
{
    if (_scene)
    {
        _scene->GetCoroutineScheduler()->StopAllCoroutines(this);
    }
}

// Utility methods
bool GameObject::IsChildOf(const Ptr &potentialParent) const
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

bool GameObject::IsParentOf(const Ptr &potentialChild)
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
    // Create GameObject with UUID. Required keys use at(): const operator[] on a missing key is an
    // assertion/UB in nlohmann, while at() throws, which Scene::FromJSON turns into a failed load.
    Math::UUID uuid = j.at("uuid").get<Math::UUID>();
    auto go = std::make_shared<GameObject>(j.at("name").get<std::string>());
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
        go->_prefabReference = j["prefabReference"].get<Math::UUID>();
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
            const std::string typeName = compJson.at("type").get<std::string>();
            const json &data = compJson.at("data");

            if (auto component = ComponentRegistry::Instance().Create(typeName, *go))
            {
                if (resolver && data.contains("uuid"))
                {
                    Math::UUID compUUID = data.at("uuid").get<Math::UUID>();
                    resolver->RegisterComponent(compUUID, component.get());
                }

                component->Deserialize(data, resolver);

                auto *rawPtr = component.get();
                std::type_index typeIdx(typeid(*rawPtr));

                go->_components.push_back(std::move(component));
                go->_componentMap.emplace(typeIdx, rawPtr); // the first of a type stays the one GetComponent finds
            }
            else
            {
                // Was skipped silently, so a component missing REGISTER_COMPONENT vanished on every load
                Logger::Warn(std::format("Unknown component type '{}' on '{}' was not loaded", typeName, go->_name));
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
