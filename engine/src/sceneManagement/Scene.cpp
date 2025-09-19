#include "engine/sceneManagement/Scene.hpp"
#include "engine/GameObject.hpp"
#include <algorithm>

using namespace N2Engine;

Scene::Scene(const std::string &name)
    : sceneName(name), _rootGameObjects()
{
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

    // handle rendering

    for (const auto &child : gameObject->GetChildren())
    {
        RenderRecursive(child, renderer);
    }
}

void Scene::AddRootGameObject(std::shared_ptr<GameObject> gameObject)
{
    if (!gameObject || gameObject->GetParent())
    {
        // Don't add null objects or objects that already have a parent
        return;
    }

    // Check if already in scene
    auto it = std::find(_rootGameObjects.begin(), _rootGameObjects.end(), gameObject);
    if (it == _rootGameObjects.end())
    {
        _rootGameObjects.push_back(gameObject);
        gameObject->SetScene(this); // Assuming you have this method
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

    if (!gameObject->GetParent())
    {
        return RemoveRootGameObject(gameObject);
    }

    auto parent = gameObject->GetParent();
    if (parent)
    {
        parent->RemoveChild(gameObject);
        return true;
    }

    return false;
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

void Scene::AddComponentToAttachQueue(std::shared_ptr<Component> component)
{
    _attachQueue.push(component);
}

void Scene::Update()
{
    // Update all active GameObjects in hierarchy order
    TraverseAllActive([](std::shared_ptr<GameObject> gameObject)
                      {
                          // Call update on components, handle transforms, etc.
                          // This ensures parent objects are updated before children
                      });
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