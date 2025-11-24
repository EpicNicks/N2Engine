#include <algorithm>
#include <memory>
#include <utility>
#include <format>

#include "engine/sceneManagement/Scene.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/IRenderable.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"

using namespace N2Engine;

Scene::Scene(std::string name)
    : _coroutineScheduler(std::make_unique<Scheduling::CoroutineScheduler>(this)), sceneName(std::move(name)) {}

Scene::~Scene() = default;
Scene::Scene(Scene &&) noexcept = default;
Scene& Scene::operator=(Scene &&) noexcept = default;

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

    for (const auto renderableComponents = gameObject->GetComponents<IRenderable>(); const auto renderable :
         renderableComponents)
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

    auto it = std::ranges::find(_rootGameObjects, gameObject);
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
    if (const auto it = std::ranges::find(_rootGameObjects, gameObject); it != _rootGameObjects.end())
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
                                        const bool onlyActive) const
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
        }
    });

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
    {
        allObjects.push_back(gameObject);
    });

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

void Scene::OnAllActiveComponents(const std::function<void(Component *)> &callback) const
{
    for (const auto &c : _components)
    {
        if (c->GetGameObject().IsActiveInHierarchy() && c->GetIsActive())
        {
            callback(c);
        }
    }
}

void Scene::AddComponentToAttachQueue(Component *component)
{
    _attachQueue.push(component);
}

void Scene::ProcessAttachQueue()
{
    while (!_attachQueue.empty())
    {
        Component *c = _attachQueue.front();
        _attachQueue.pop();
        c->OnAttach();

        // attach has been called, now can be updated
        _components.push_back(c);
        if (auto *light = dynamic_cast<Rendering::Light*>(c))
        {
            _sceneLights.push_back(light);
        }
    }
}

void Scene::Update() const
{
    OnAllActiveComponents([](Component *component)
    {
        component->OnUpdate();
    });
}

void Scene::FixedUpdate() const
{
    OnAllActiveComponents([](Component *component)
    {
        component->OnFixedUpdate();
    });
}

void Scene::LateUpdate() const
{
    OnAllActiveComponents([](Component *component)
    {
        component->OnLateUpdate();
    });
}

void Scene::AdvanceCoroutines() const
{
    _coroutineScheduler->Update();
}

void Scene::OnApplicationQuit() const
{
    OnAllActiveComponents([](Component *component)
    {
        component->OnApplicationQuit();
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

void Scene::MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject,
                                        std::vector<std::shared_ptr<GameObject>> &markedObjects)
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
    if (const auto parent = gameObject->GetParent())
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
    std::erase_if(_components,
                  [&gameObject](const Component *comp)
                  {
                      return &comp->GetGameObject() == gameObject.get();
                  });

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

Renderer::Common::SceneLightingData Scene::CollectLighting() const
{
    using namespace Renderer::Common;

    SceneLightingData lighting;

    // Iterate through cached scene lights
    for (auto *light : _sceneLights)
    {
        if (!light || !light->GetIsActive())
        {
            continue;
        }

        switch (light->type)
        {
        case Rendering::LightType::Directional:
            {
                if (lighting.directionalLights.size() < SceneLightingData::MAX_DIRECTIONAL_LIGHTS)
                {
                    DirectionalLightData data;
                    data.direction = light->GetWorldDirection();
                    data.color = light->color;
                    data.intensity = light->intensity;
                    lighting.directionalLights.push_back(data);
                }
                break;
            }

        case Rendering::LightType::Point:
            {
                if (lighting.pointLights.size() < SceneLightingData::MAX_POINT_LIGHTS)
                {
                    PointLightData data;
                    data.position = light->GetWorldPosition();
                    data.color = light->color;
                    data.intensity = light->intensity;
                    data.range = light->range;
                    data.attenuation = light->attenuation;
                    lighting.pointLights.push_back(data);
                }
                break;
            }

        case Rendering::LightType::Spot:
            {
                if (lighting.spotLights.size() < SceneLightingData::MAX_SPOT_LIGHTS)
                {
                    SpotLightData data;
                    data.position = light->GetWorldPosition();
                    data.direction = light->GetWorldDirection();
                    data.color = light->color;
                    data.intensity = light->intensity;
                    data.range = light->range;
                    // Convert degrees to radians
                    data.innerConeAngle = light->innerConeAngle * (3.14159f / 180.0f);
                    data.outerConeAngle = light->outerConeAngle * (3.14159f / 180.0f);
                    lighting.spotLights.push_back(data);
                }
                break;
            }
        }
    }

    // If no lights in scene, add a default directional light
    if (lighting.directionalLights.empty() && lighting.pointLights.empty() && lighting.spotLights.empty())
    {
        if (!_hasWarnedNoLights)
        {
            Logger::Warn(std::format("No lights found in scene '{}'. Using default light.", sceneName));
            _hasWarnedNoLights = true;
        }

        DirectionalLightData defaultLight;
        defaultLight.direction = Math::Vector3{0.5f, -1.0f, 0.3f};
        defaultLight.color = Math::Vector3{1.0f, 1.0f, 1.0f};
        defaultLight.intensity = 0.8f;
        lighting.directionalLights.push_back(defaultLight);
    }

    return lighting;
}

Scheduling::CoroutineScheduler* Scene::GetCoroutineScheduler() const
{
    return _coroutineScheduler.get();
}
