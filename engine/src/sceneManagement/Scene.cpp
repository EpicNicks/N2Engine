#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <format>

#include <math/UUID.hpp>

#include "engine/sceneManagement/Scene.hpp"
#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/Camera.hpp"
#include "engine/IRenderable.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"

using namespace N2Engine;

Scene::Scene(std::string name)
    : _coroutineScheduler(std::make_unique<Scheduling::CoroutineScheduler>(this)), sceneName(std::move(name)) {}

Scene::~Scene()
{
    // A scene dropped without being loaded (a superseded pending instance, a GetScene copy, the scene
    // AddScene(scene, false) snapshots) is normally never attached, so this is a no-op. If its owner did
    // run ProcessAttachQueue on it, the components that attached get the normal Clear teardown, so what
    // their OnAttach acquired (physics bodies, audio sources, script subscriptions) is released. Components
    // that never attached get no OnDisable/OnDestroy, as there is no setup to undo.
    if (!_skipDropTeardown && HasAttachedComponents())
    {
        // A destructor must not throw: a throwing callback ends the teardown here (as it would end Clear)
        try
        {
            ClearImpl(true);
        }
        catch (const std::exception &e)
        {
            // Logging must not throw out of the destructor either (that would terminate)
            try
            {
                Logger::Error(std::format("Scene '{}': teardown of a dropped scene threw: {}", sceneName, e.what()));
            }
            catch (...) {}
        }
        catch (...)
        {
            try
            {
                Logger::Error(std::format("Scene '{}': teardown of a dropped scene threw", sceneName));
            }
            catch (...) {}
        }
    }

    // Objects can outlive their scene (held by a script, or removed from the hierarchy without leaving
    // it). Cut every one loose, so none keeps a pointer to this scene for its destructor to use.
    // No callbacks run: this only updates the bookkeeping.
    std::vector<GameObject *> members;
    for (const auto &root : _rootGameObjects)
    {
        members.push_back(root.get());
    }
    for (const auto *list : {&_components, &_attachQueue})
    {
        for (const Component *component : *list)
        {
            if (component)
            {
                members.push_back(&component->GetGameObject());
            }
        }
    }
    for (GameObject *member : members)
    {
        if (member->GetScene() == this)
        {
            member->SetScene(nullptr);
        }
    }
}


std::unique_ptr<Scene> Scene::Create(const std::string &name)
{
    return std::unique_ptr<Scene>(new Scene{name});
}

void Scene::Render(Renderer::Common::IRenderer *renderer, const Camera &camera)
{
    if (!renderer)
    {
        return;
    }

    // One traversal, in the order rendering has always used
    std::vector<IRenderable *> renderables;
    for (const auto &rootObject : _rootGameObjects)
    {
        CollectRenderablesRecursive(rootObject, renderables);
    }

    struct TransparentEntry
    {
        IRenderable *renderable;
        int sortKey;
        float depth; // distance in front of the camera, along its view direction
    };

    // Split by queue before anything draws. Opaque keeps the traversal order exactly. A renderable that
    // draws in both queues (DrawsInQueue) goes in both lists.
    std::vector<IRenderable *> opaque;
    std::vector<TransparentEntry> transparent;
    opaque.reserve(renderables.size());
    const Matrix4 &view = camera.GetViewMatrix();
    for (IRenderable *renderable : renderables)
    {
        const RenderQueueKey key = renderable->GetRenderQueue();
        if (renderable->DrawsInQueue(RenderQueue::Opaque))
        {
            opaque.push_back(renderable);
        }
        if (!renderable->DrawsInQueue(RenderQueue::Transparent))
        {
            continue;
        }

        float depth = 0.0f;
        if (const Positionable *positionable = renderable->GetGameObject().GetPositionable())
        {
            // The camera looks down its -Z axis, so view-space z is minus the depth
            const Math::Vector3 p = positionable->GetPosition();
            depth = -(view(2, 0) * p.x + view(2, 1) * p.y + view(2, 2) * p.z + view(2, 3));
            if (!std::isfinite(depth))
            {
                depth = 0.0f; // keeps the sort's ordering strict
            }
        }
        transparent.push_back(TransparentEntry{renderable, key.sortKey, depth});
    }

    // Lower sort keys first; within a key, back to front
    std::ranges::stable_sort(transparent, [](const TransparentEntry &a, const TransparentEntry &b) {
        if (a.sortKey != b.sortKey)
        {
            return a.sortKey < b.sortKey;
        }
        return a.depth > b.depth;
    });

    for (IRenderable *renderable : opaque)
    {
        renderable->RenderInQueue(renderer, Renderer::Common::RenderState::Opaque(), RenderQueue::Opaque);
    }
    for (const TransparentEntry &entry : transparent)
    {
        entry.renderable->RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(),
                                        RenderQueue::Transparent);
    }
}

void Scene::CollectRenderablesRecursive(const std::shared_ptr<GameObject> &gameObject, std::vector<IRenderable *> &out)
{
    if (gameObject == nullptr || !gameObject->IsActiveInHierarchy())
    {
        return;
    }

    for (const auto renderableComponents = gameObject->GetComponents<IRenderable>(); const auto renderable :
         renderableComponents)
    {
        if (renderable && renderable->IsActive())
        {
            out.push_back(renderable);
        }
    }

    for (const auto &child : gameObject->GetChildren())
    {
        CollectRenderablesRecursive(child, out);
    }
}

void Scene::AddRootGameObject(std::shared_ptr<GameObject> gameObject)
{
    if (!gameObject || gameObject->GetParent())
    {
        return;
    }

    // Moving a root here from another scene: it used to stay in that scene's roots as well
    if (Scene *oldScene = gameObject->GetScene(); oldScene && oldScene != this)
    {
        std::erase(oldScene->_rootGameObjects, gameObject);
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
    // Another scene's object (or none's) would be purged from the wrong root list and scheduler
    if (gameObject->GetScene() != this)
    {
        Logger::Warn(std::format("DestroyGameObject: '{}' isn't in scene '{}'", gameObject->GetName(), sceneName));
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

std::shared_ptr<GameObject> Scene::FindGameObjectWithTag(const std::string &tag) const
{
    std::shared_ptr<GameObject> result = nullptr;

    TraverseUntil([&](const std::shared_ptr<GameObject> &gameObject)
    {
        if (gameObject->CompareTag(tag))
        {
            result = gameObject;
            return true;
        }
        return false;
    });

    return result;
}

std::vector<std::shared_ptr<GameObject>> Scene::FindGameObjectsWithTag(const std::string &tag) const
{
    std::vector<std::shared_ptr<GameObject>> results;

    TraverseAll([&](const std::shared_ptr<GameObject> &gameObject)
    {
        if (gameObject->CompareTag(tag))
        {
            results.push_back(gameObject);
        }
    });

    return results;
}

std::vector<std::shared_ptr<GameObject>> Scene::FindGameObjectsByTag(const std::string &tag) const
{
    return FindGameObjectsWithTag(tag);
}

std::shared_ptr<GameObject> Scene::FindGameObjectByUUID(const Math::UUID uuid)
{
    std::shared_ptr<GameObject> result = nullptr;

    TraverseAll([&](std::shared_ptr<GameObject> gameObject)
    {
        if (!result && gameObject->GetUUID() == uuid)
        {
            result = gameObject;
        }
    });

    return result;
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
    // By index: callbacks can add components (appended, picked up this pass) or remove them
    // (nulled by DetachComponent while iterating), both of which would invalidate iterators
    // Scope guard: the depth must come back down (and nulls be compacted) even if a callback throws
    struct IterationScope
    {
        const Scene &scene;
        explicit IterationScope(const Scene &s) : scene(s) { ++scene._componentIterationDepth; }
        ~IterationScope()
        {
            if (--scene._componentIterationDepth == 0)
            {
                std::erase(scene._components, nullptr);
            }
        }
        IterationScope(const IterationScope &) = delete;
        IterationScope &operator=(const IterationScope &) = delete;
    } scope{*this};

    for (std::size_t i = 0; i < _components.size(); ++i)
    {
        Component *c = _components[i];
        if (c && c->GetGameObject().IsActiveInHierarchy() && c->IsActive())
        {
            callback(c);
        }
    }
}

void Scene::AddComponentToAttachQueue(Component *component)
{
    if (std::ranges::find(_attachQueue, component) == _attachQueue.end())
    {
        _attachQueue.push_back(component);
    }
}

void Scene::DetachComponent(Component *component)
{
    if (_componentIterationDepth > 0)
    {
        // A component's update removed a component: null the entry so the running loop skips it,
        // and let the loop compact the vector once it finishes
        std::ranges::replace(_components, component, static_cast<Component *>(nullptr));
    }
    else
    {
        std::erase(_components, component);
    }
    std::erase(_attachQueue, component);
    // Tells ProcessAttachQueue that OnAttach removed it (or moved its object away)
    std::ranges::replace(_attaching, component, static_cast<Component *>(nullptr));
    std::erase_if(_sceneLights, [component](const Rendering::Light *light)
    {
        return static_cast<const Component *>(light) == component;
    });
}

void Scene::ProcessAttachQueue()
{
    // Take one entry at a time from the live queue: OnAttach can queue more components, or remove
    // pending ones (DetachComponent erases them from _attachQueue), and neither may leave a stale pointer
    while (!_attachQueue.empty())
    {
        Component *c = _attachQueue.front();
        _attachQueue.erase(_attachQueue.begin());

        // OnAttach can remove (free) this component or move its object to another scene; either way
        // DetachComponent nulls its _attaching entry, and c must not be registered or touched again.
        // A stack, so an OnAttach that processes the queue again doesn't lose track of c.
        _attaching.push_back(c);
        struct AttachingScope
        {
            std::vector<Component *> &attaching;
            ~AttachingScope() { attaching.pop_back(); }
        } scope{_attaching};
        c->OnAttach();
        if (_attaching.back() != c)
        {
            continue;
        }

        // Registration is idempotent: a component re-queued after re-parenting isn't updated twice.
        // Lights get the per-frame callbacks like any component, and are also kept in the light list
        // that rendering reads.
        if (std::ranges::find(_components, c) == _components.end())
        {
            _components.push_back(c);
        }
        if (auto *light = dynamic_cast<Rendering::Light*>(c))
        {
            if (std::ranges::find(_sceneLights, light) == _sceneLights.end())
            {
                _sceneLights.push_back(light);
            }
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
    ClearImpl(false);
}

bool Scene::HasAttachedComponents() const
{
    return std::ranges::any_of(_components, [](const Component *component) { return component != nullptr; });
}

void Scene::ClearImpl(const bool attachedOnly)
{
    // Same teardown as ProcessDestroyed, so leaving a scene releases what its components hold
    // (physics bodies, audio sources, script state) instead of leaving it pointing at freed objects
    // attachedOnly: only objects with a component that attached here are torn down; the others are just
    // released from the scene, as they would be if nothing in the scene had attached
    std::vector<const GameObject *> attachedOwners;
    if (attachedOnly)
    {
        for (const Component *component : _components)
        {
            if (component)
            {
                attachedOwners.push_back(&component->GetGameObject());
            }
        }
    }
    const auto isTornDownHere = [&](const std::shared_ptr<GameObject> &obj)
    {
        return !attachedOnly || std::ranges::find(attachedOwners, obj.get()) != attachedOwners.end();
    };

    std::vector<std::shared_ptr<GameObject>> allObjects;
    for (const auto &root : _rootGameObjects)
    {
        MarkHierarchyForDestruction(root, allObjects);
    }
    for (const auto &obj : allObjects)
    {
        CallOnDestroyForGameObject(obj, attachedOnly);
    }
    // Only now, so each OnDestroy above could still use the other objects being torn down
    for (const auto &obj : allObjects)
    {
        if (isTornDownHere(obj))
        {
            obj->_isTornDown = true;
        }
        else
        {
            obj->_isMarkedForDestruction = false;
            obj->MarkActiveInHierarchyDirty();
        }
    }

    for (const auto &root : _rootGameObjects)
    {
        root->SetScene(nullptr); // detaches every component in the hierarchy
    }
    // And every torn-down object, under a root or not: one an OnDestroy above unlinked from its parent
    // would otherwise keep pointing at this scene after it's freed. (One it moved to another scene stays.)
    for (const auto &obj : allObjects)
    {
        if (obj->GetScene() == this)
        {
            obj->SetScene(nullptr);
        }
    }
    _rootGameObjects.clear();
    _components.clear();
    _attachQueue.clear();
    _sceneLights.clear();
    _markedForDestructionQueue = {};
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
        // Moved to another scene since it was queued: purging it here would detach its components from
        // that scene while it stayed one of its roots
        if (rootObject->GetScene() != this)
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

void Scene::CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject, const bool attachedOnly)
{
    // Objects are already marked for destruction here, so ask whether they were active before that
    const bool wasActive = gameObject->IsActiveInHierarchyIgnoringDestruction();
    // A snapshot: the callbacks can add or remove this object's components, which invalidated the live
    // iteration; one removed (freed) before its turn is skipped
    for (Component *component : gameObject->SnapshotComponents())
    {
        // Runs OnDisable/OnDestroy exactly once, whichever teardown path gets here first.
        // attachedOnly (a dropped scene): a component that never attached here has nothing to undo
        if (attachedOnly && std::ranges::find(_components, component) == _components.end())
        {
            continue;
        }
        if (gameObject->OwnsComponent(component))
        {
            component->RunDestroyCallbacks(wasActive);
        }
    }
    // This scene's scheduler, not the loaded scene's: the object may belong to a scene being cleared
    _coroutineScheduler->StopAllCoroutines(gameObject.get());
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
        std::erase(_rootGameObjects, gameObject);
    }

    // Detach from this scene (components out of _components, _sceneLights and the attach queue),
    // then release the components themselves
    gameObject->SetScene(nullptr);
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

void Scene::Deserialize(const json &j)
{
    if (j.contains("name"))
    {
        sceneName = j["name"];
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
            _rootGameObjects.push_back(root);
            root->SetScene(this);
        }
    }

    // Phase 2: Resolve all references
    // This fills in all the GameObject and Component references
    resolver.ResolveAll();
}

std::unique_ptr<Scene> Scene::FromJSON(const nlohmann::json &j, bool validate)
{
    if (validate)
    {
        if (!j.contains("name") || !j.contains("rootGameObjects") || !j["rootGameObjects"].is_array())
        {
            Logger::Error("Invalid scene json provided, missing name or rootGameObjects\n" + j.dump());
            return nullptr;
        }
        // GameObjects being invalid will error if the Component type is not present in the resolver
    }
    auto scene = std::unique_ptr<Scene>(new Scene(""));
    try
    {
        scene->Deserialize(j);
    }
    catch (const std::exception &e)
    {
        // Malformed data (missing keys, wrong types) fails this load instead of aborting the engine
        Logger::Error(std::format("Failed to load scene: {}", e.what()));
        return nullptr;
    }
    return scene;
}

std::string Scene::GetResourceType() const
{
    return std::string(ResourceTypeName);
}

void Scene::CollectLightsRecursive(const std::shared_ptr<GameObject> &gameObject, std::vector<Rendering::Light *> &out)
{
    if (gameObject == nullptr || !gameObject->IsActiveInHierarchy())
    {
        return;
    }

    for (const auto light : gameObject->GetComponents<Rendering::Light>())
    {
        if (light && light->IsActive())
        {
            out.push_back(light);
        }
    }

    for (const auto &child : gameObject->GetChildren())
    {
        CollectLightsRecursive(child, out);
    }
}

Renderer::Common::SceneLightingData Scene::CollectLighting() const
{
    return CollectLighting(_editMode ? LightingSource::Hierarchy : LightingSource::Attached);
}

Renderer::Common::SceneLightingData Scene::CollectLighting(const LightingSource source) const
{
    using namespace Renderer::Common;

    SceneLightingData lighting;
    lighting.colorSpace = Rendering::RenderSettings::GetColorSpace();

    // The registered lights (attached ones), or, in edit mode, the ones the hierarchy holds
    std::vector<Rendering::Light *> hierarchyLights;
    if (source == LightingSource::Hierarchy)
    {
        for (const auto &root : _rootGameObjects)
        {
            CollectLightsRecursive(root, hierarchyLights);
        }
    }
    const std::vector<Rendering::Light *> &lights =
        source == LightingSource::Hierarchy ? hierarchyLights : _sceneLights;

    for (auto *light : lights)
    {
        if (!light || !light->IsActive())
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
