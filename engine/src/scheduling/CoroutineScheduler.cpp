#include <algorithm>
#include <exception>
#include <format>
#include <generator>

#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/Logger.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/GameObjectScene.hpp"

using namespace N2Engine::Scheduling;

CoroutineScheduler::CoroutineScheduler(Scene *scene)
    : _scene(scene) {}

void CoroutineScheduler::Update()
{
    // Coroutines stop when their object is deactivated or destroyed. (The predicate used to be
    // inverted, so every coroutine on an *active* object was erased before it first ran.)
    for (auto &[gameObject, coroutineList] : _coroutines)
    {
        if (!gameObject->IsActiveInHierarchy() || gameObject->IsDestroyed())
        {
            for (const auto &coroutine : coroutineList)
            {
                coroutine->RequestStop();
            }
        }
    }
    RemoveFinished();

    // Snapshot: a coroutine body can start coroutines (run from next frame) or stop them, and either
    // would invalidate iterators over the live containers
    std::vector<std::pair<GameObject *, Coroutine *>> snapshot;
    for (const auto &[gameObject, coroutineList] : _coroutines)
    {
        for (const auto &coroutine : coroutineList)
        {
            snapshot.emplace_back(gameObject, coroutine.get());
        }
    }

    _updating = true;
    for (const auto &[gameObject, coroutine] : snapshot)
    {
        // Stops during this update are deferred, so the pointer is still valid; skip if it was stopped
        if (Contains(gameObject, coroutine) && !coroutine->IsStopRequested() && !AdvanceCoroutine(coroutine))
        {
            coroutine->RequestStop(); // finished: remove below
        }
    }
    _updating = false;

    RemoveFinished();
}

Coroutine* CoroutineScheduler::StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator)
{
    if (!gameObject || !gameObject->IsActiveInHierarchy())
    {
        return nullptr;
    }

    auto &coroutineList = _coroutines[gameObject];
    coroutineList.push_back(std::make_unique<Coroutine>(std::move(generator)));
    return coroutineList.back().get();
}

bool CoroutineScheduler::StopCoroutine(GameObject *gameObject, Coroutine *coroutine)
{
    // Stopping works whether or not the object is active
    if (!gameObject || !Contains(gameObject, coroutine) || coroutine->IsStopRequested())
    {
        return false;
    }

    coroutine->RequestStop();
    if (!_updating)
    {
        RemoveFinished();
    }
    return true;
}

void CoroutineScheduler::StopAllCoroutines(GameObject *gameObject)
{
    const auto it = _coroutines.find(gameObject);
    if (it == _coroutines.end())
    {
        return;
    }

    for (const auto &coroutine : it->second)
    {
        coroutine->RequestStop();
    }
    if (!_updating)
    {
        // Drop the entry now, so no key refers to an object that's about to be freed
        _coroutines.erase(it);
    }
}

Coroutine* CoroutineScheduler::StartCoroutine(const Scene *curScene, GameObject *gameObject,
                                              std::generator<ICoroutineWait> &&generator)
{
    return curScene->GetCoroutineScheduler()->StartCoroutine(gameObject, std::move(generator));
}

bool CoroutineScheduler::StopCoroutine(const Scene *curScene, GameObject *gameObject, Coroutine *coroutine)
{
    return curScene->GetCoroutineScheduler()->StopCoroutine(gameObject, coroutine);
}

void CoroutineScheduler::StopAllCoroutines(const Scene *curScene, GameObject *gameObject)
{
    curScene->GetCoroutineScheduler()->StopAllCoroutines(gameObject);
}

bool CoroutineScheduler::RemoveGameObject(GameObject *gameObject)
{
    if (!_coroutines.contains(gameObject))
    {
        return false;
    }
    StopAllCoroutines(gameObject);
    return true;
}

bool CoroutineScheduler::AdvanceCoroutine(Coroutine *coroutine)
{
    try
    {
        return coroutine->MoveNext();
    }
    catch (const std::exception &e)
    {
        Logger::Error(std::format("Coroutine threw and was stopped: {}", e.what()));
    }
    catch (...)
    {
        Logger::Error("Coroutine threw a non-standard exception and was stopped");
    }
    return false;
}

bool CoroutineScheduler::Contains(GameObject *gameObject, const Coroutine *coroutine) const
{
    const auto it = _coroutines.find(gameObject);
    return it != _coroutines.end() &&
           std::ranges::any_of(it->second, [coroutine](const auto &c) { return c.get() == coroutine; });
}

void CoroutineScheduler::RemoveFinished()
{
    for (auto it = _coroutines.begin(); it != _coroutines.end();)
    {
        std::erase_if(it->second, [](const std::unique_ptr<Coroutine> &c)
        {
            return c->IsStopRequested() || c->IsComplete();
        });
        it = it->second.empty() ? _coroutines.erase(it) : std::next(it);
    }
}
