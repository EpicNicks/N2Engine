#include <algorithm>
#include <exception>
#include <format>
#include <generator>
#include <ranges>

#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/Logger.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/GameObjectScene.hpp"

using namespace N2Engine::Scheduling;

CoroutineScheduler::CoroutineScheduler(Scene *scene)
    : _scene(scene) {}

void CoroutineScheduler::Update()
{
    if (_updating)
    {
        // Re-entered from a coroutine body (something that ticks the scene): the outer update's
        // snapshot still owns these coroutines, and one of them is executing
        return;
    }

    // Coroutines stop when their object is deactivated or destroyed. (The predicate used to be
    // inverted, so every coroutine on an *active* object was erased before it first ran.)
    for (auto &entry : _coroutines | std::views::values)
    {
        GameObject *gameObject = entry.gameObject;
        if (!gameObject->IsActiveInHierarchy() || gameObject->IsDestroyed())
        {
            for (const auto &coroutine : entry.coroutines)
            {
                coroutine->RequestStop();
            }
        }
    }
    RemoveFinished();

    // Snapshot: a coroutine body can start coroutines (run from next frame) or stop them, and either
    // would invalidate iterators over the live containers
    std::vector<std::pair<GameObject *, Coroutine *>> snapshot;
    for (const auto &entry : _coroutines | std::views::values)
    {
        for (const auto &coroutine : entry.coroutines)
        {
            snapshot.emplace_back(entry.gameObject, coroutine.get());
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

    auto [sequence, isNew] = _sequenceOf.try_emplace(gameObject, _nextSequence);
    if (isNew)
    {
        ++_nextSequence;
        _coroutines.emplace(sequence->second, Entry{gameObject, {}});
    }
    auto &coroutineList = _coroutines.at(sequence->second).coroutines;
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
    const auto sequence = _sequenceOf.find(gameObject);
    if (sequence == _sequenceOf.end())
    {
        return;
    }

    const auto it = _coroutines.find(sequence->second);
    for (const auto &coroutine : it->second.coroutines)
    {
        coroutine->RequestStop();
    }
    if (!_updating)
    {
        // Drop the entry now, so no key refers to an object that's about to be freed
        _coroutines.erase(it);
        _sequenceOf.erase(sequence);
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
    if (!_sequenceOf.contains(gameObject))
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
    const auto *list = Find(gameObject);
    return list && std::ranges::any_of(*list, [coroutine](const auto &c) { return c.get() == coroutine; });
}

const std::vector<std::unique_ptr<Coroutine>>* CoroutineScheduler::Find(GameObject *gameObject) const
{
    const auto sequence = _sequenceOf.find(gameObject);
    return sequence == _sequenceOf.end() ? nullptr : &_coroutines.at(sequence->second).coroutines;
}

void CoroutineScheduler::RemoveFinished()
{
    for (auto it = _coroutines.begin(); it != _coroutines.end();)
    {
        std::erase_if(it->second.coroutines, [](const std::unique_ptr<Coroutine> &c)
        {
            return c->IsStopRequested() || c->IsComplete();
        });
        if (it->second.coroutines.empty())
        {
            _sequenceOf.erase(it->second.gameObject);
            it = _coroutines.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
