#include "engine/sceneManagement/Scene.hpp"
#include "engine/GameObject.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/IRenderable.hpp"
#include "engine/GameObject.inl"
#include "engine/serialization/ReferenceResolver.hpp"

#include <algorithm>
#include <memory>

using namespace N2Engine;

Scene::Scene(const std::string &name)
    : sceneName(name), _rootGameObjects()
{
}

std::unique_ptr<Scene> Scene::Create(const std::string &name)
{
    return std::unique_ptr<Scene>(new Scene{name});
}

void Scene::Render(Renderer::Common::IRenderer *renderer)
{
    // Render all root GameObjects (which will recursively render their children)
    for (const auto &rootObject : _rootGameObjects)
    {
        if (rootObject->IsActiveInHierarchy())
        {
            RenderRecursive(rootObject, renderer);
        }
    }
}

void Scene::RenderRecursive(std::shared_ptr<GameObject> gameObject, Renderer::Common::IRenderer *renderer)
{
    if (gameObject == nullptr || !gameObject->IsActiveInHierarchy())
    {
        return;
    }

    auto renderableComponents = gameObject->GetComponents<IRenderable>();
    for (auto &renderable : renderableComponents)
    {
        if (renderable && renderable->GetIsActive())
        {
            renderable->Render(renderer);
        }
    }

    for (const auto &child : gameObject->GetChildren())
    {
        RenderRecursive(child, renderer);
    }
}

void Scene::AddRootGameObject(std::shared_ptr<GameObject> gameObject)
{
    if (!gameObject || gameObject->GetParent())
    {
        return;
    }

    auto it = std::find(_rootGameObjects.begin(), _rootGameObjects.end(), gameObject);
    if (it == _rootGameObjects.end())
    {
        _rootGameObjects.push_back(gameObject);
        gameObject->SetScene(this);
    }
}

void Scene::AddRootGameObjects(const std::vector<std::shared_ptr<GameObject>> &gameObjects)
{
    for (const auto &gameObject : gameObjects)
    {
        AddRootGameObject(gameObject);
    }
}

void Scene::AddRootGameObjects(std::initializer_list<std::shared_ptr<GameObject>> gameObjects)
{
    for (const auto &gameObject : gameObjects)
    {
        AddRootGameObject(gameObject);
    }
}

bool Scene::RemoveRootGameObject(std::shared_ptr<GameObject> gameObject)
{
    auto it = std::find(_rootGameObjects.begin(), _rootGameObjects.end(), gameObject);
    if (it != _rootGameObjects.end())
    {
        (*it)->SetScene(nullptr);
        _rootGameObjects.erase(it);
        return true;
    }
    return false;
}

bool Scene::DestroyGameObject(std::shared_ptr<GameObject> gameObject)
{
    if (!gameObject)
    {
        return false;
    }

    _markedForDestructionQueue.push(gameObject);
    return true;
}

void Scene::TraverseAll(std::function<void(std::shared_ptr<GameObject>)> callback) const
{
    for (const auto &root : _rootGameObjects)
    {
        TraverseGameObjectRecursive(root, callback, false);
    }
}

void Scene::TraverseAllActive(std::function<void(std::shared_ptr<GameObject>)> callback) const
{
    for (const auto &root : _rootGameObjects)
    {
        if (root->IsActiveInHierarchy())
        {
            TraverseGameObjectRecursive(root, callback, true);
        }
    }
}

void Scene::TraverseGameObjectRecursive(std::shared_ptr<GameObject> gameObject,
                                        std::function<void(std::shared_ptr<GameObject>)> callback,
                                        bool onlyActive) const
{
    if (!gameObject || (onlyActive && !gameObject->IsActiveInHierarchy()))
    {
        return;
    }

    // Process current object
    callback(gameObject);

    // Process children
    for (const auto &child : gameObject->GetChildren())
    {
        TraverseGameObjectRecursive(child, callback, onlyActive);
    }
}

std::shared_ptr<GameObject> Scene::FindGameObject(const std::string &name) const
{
    std::shared_ptr<GameObject> result = nullptr;

    TraverseAll([&](std::shared_ptr<GameObject> gameObject)
                {
        if (!result && gameObject->GetName() == name)
        {
            result = gameObject;
        } });

    return result;
}

std::vector<std::shared_ptr<GameObject>> Scene::FindGameObjectsByTag(const std::string &tag) const
{
    std::vector<std::shared_ptr<GameObject>> results;

    TraverseAll([&](std::shared_ptr<GameObject> gameObject)
                {
                    // Assuming you have a tag system in GameObject
                    // if (gameObject->GetTag() == tag)
                    // {
                    //     results.push_back(gameObject);
                    // }
                });

    return results;
}

std::vector<std::shared_ptr<GameObject>> Scene::GetAllGameObjects() const
{
    std::vector<std::shared_ptr<GameObject>> allObjects;

    TraverseAll([&](std::shared_ptr<GameObject> gameObject)
                { allObjects.push_back(gameObject); });

    return allObjects;
}

bool Scene::TraverseUntil(std::function<bool(std::shared_ptr<GameObject>)> callback) const
{
    for (const auto &root : _rootGameObjects)
    {
        if (TraverseGameObjectUntil(root, callback))
        {
            return true; // Early exit from entire traversal
        }
    }
    return false;
}

bool Scene::TraverseGameObjectUntil(std::shared_ptr<GameObject> gameObject,
                                    std::function<bool(std::shared_ptr<GameObject>)> callback) const
{
    if (callback(gameObject))
    {
        return true;
    }

    for (const auto &child : gameObject->GetChildren())
    {
        if (TraverseGameObjectUntil(child, callback))
        {
            return true;
        }
    }

    return false;
}

void Scene::OnAllActiveComponents(std::function<void(std::shared_ptr<Component>)> callback)
{
    for (const auto &c : _components)
    {
        if (c->GetGameObject().IsActiveInHierarchy() && c->GetIsActive())
        {
            callback(c);
        }
    }
}

void Scene::AddComponentToAttachQueue(std::shared_ptr<Component> component)
{
    _attachQueue.push(component);
}

void Scene::ProcessAttachQueue()
{
    while (!_attachQueue.empty())
    {
        std::shared_ptr<Component> c = _attachQueue.front();
        _attachQueue.pop();
        c->OnAttach();

        // attach has been called, now can be updated
        _components.push_back(c);
    }
}

void Scene::Update()
{
    OnAllActiveComponents([](std::shared_ptr<Component> component)
                          { component->OnUpdate(); });
}

void Scene::FixedUpdate()
{
    OnAllActiveComponents([](std::shared_ptr<Component> component)
                          { component->OnFixedUpdate(); });
}

void Scene::LateUpdate()
{
    OnAllActiveComponents([](std::shared_ptr<Component> component)
                          { component->OnLateUpdate(); });
}

void Scene::AdvanceCoroutines()
{
    Scheduling::CoroutineScheduler::Update();
}

void Scene::OnApplicationQuit()
{
    OnAllActiveComponents([](std::shared_ptr<Component> component)
                          { component->OnApplicationQuit(); });
}

void Scene::Clear()
{
    // Clear all root objects (this will naturally clear their children too)
    for (auto &root : _rootGameObjects)
    {
        root->SetScene(nullptr);
    }
    _rootGameObjects.clear();
}

void Scene::ProcessDestroyed()
{
    std::vector<std::shared_ptr<GameObject>> markedObjects;

    while (!_markedForDestructionQueue.empty())
    {
        auto rootObject = _markedForDestructionQueue.front();
        _markedForDestructionQueue.pop();

        if (!rootObject || rootObject->_isMarkedForDestruction)
        {
            continue;
        }

        // Mark hierarchy and collect objects in one pass
        MarkHierarchyForDestruction(rootObject, markedObjects);
    }

    for (auto &obj : markedObjects)
    {
        CallOnDestroyForGameObject(obj);
    }
    for (auto &obj : markedObjects)
    {
        PurgeMarkedGameObject(obj);
    }
}

void Scene::MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject, std::vector<std::shared_ptr<GameObject>> &markedObjects)
{
    if (!gameObject || gameObject->_isMarkedForDestruction)
        return;

    // Mark this object
    gameObject->_isMarkedForDestruction = true;

    // Collect it immediately
    markedObjects.push_back(gameObject);

    // Recursively mark and collect all children
    auto children = gameObject->GetChildren(); // Copy to avoid iterator issues
    for (auto &child : children)
    {
        MarkHierarchyForDestruction(child, markedObjects);
    }
}

void Scene::CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject)
{
    for (auto &component : gameObject->GetAllComponents())
    {
        if (component)
        {
            component->OnDestroy();
        }
    }
    gameObject->StopAllCoroutines();
    for (auto &component : gameObject->GetAllComponents())
    {
        if (component)
        {
            component->_isMarkedForDestruction = true;
        }
    }
}

void Scene::PurgeMarkedGameObject(std::shared_ptr<GameObject> gameObject)
{
    // Remove from parent or scene root
    if (auto parent = gameObject->GetParent())
    {
        // Only remove if parent isn't also being destroyed
        if (!parent->_isMarkedForDestruction)
        {
            parent->RemoveChild(gameObject, false);
        }
    }
    else
    {
        RemoveRootGameObject(gameObject);
    }

    // Remove from components list
    _components.erase(
        std::remove_if(_components.begin(), _components.end(),
                       [&gameObject](const std::shared_ptr<Component> &comp)
                       {
                           return &comp->GetGameObject() == gameObject.get();
                       }),
        _components.end());

    gameObject->Purge();
}

using json = nlohmann::json;
json Scene::Serialize() const
{
    json j;
    j["name"] = sceneName;

    json roots = json::array();
    for (const auto &root : _rootGameObjects)
    {
        roots.push_back(root->Serialize());
    }
    j["rootGameObjects"] = roots;

    return j;
}

std::shared_ptr<Scene> Scene::Deserialize(const json &j)
{
    auto scene = std::shared_ptr<Scene>(new Scene(""));

    if (j.contains("name"))
    {
        scene->sceneName = j["name"];
    }

    // Create reference resolver for this scene
    ReferenceResolver resolver;

    // Phase 1: Deserialize all GameObjects and Components
    // This creates all objects and registers them with the resolver
    if (j.contains("rootGameObjects"))
    {
        for (const auto &rootJson : j["rootGameObjects"])
        {
            auto root = GameObject::Deserialize(rootJson, &resolver);
            scene->_rootGameObjects.push_back(root);
            root->SetScene(scene.get());
        }
    }

    // Phase 2: Resolve all references
    // This fills in all the GameObject and Component references
    resolver.ResolveAll();

    return scene;
}