#include <algorithm>
#include <generator>

#include "engine/scheduling/CoroutineScheduler.hpp"

#include "engine/Application.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/GameObjectScene.hpp"

using namespace N2Engine::Scheduling;

CoroutineScheduler::CoroutineScheduler(Scene *scene)
    : _scene(scene) {}


void CoroutineScheduler::Update()
{
    std::vector<std::pair<GameObject*, Coroutine*>> coroutinesToRemove;
    CleanupInvalid();

    for (auto &[gameObject, coroutineList] : _coroutines)
    {
        for (auto &coroutine : coroutineList)
        {
            if (AdvanceCoroutine(coroutine.get()))
            {
                coroutinesToRemove.emplace_back(gameObject, coroutine.get());
            }
        }
    }
    CleanupCompleted(coroutinesToRemove);
}

Coroutine* CoroutineScheduler::StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator)
{
    if (!gameObject || !gameObject->IsActiveInHierarchy())
    {
        return nullptr;
    }
    auto routine = std::make_unique<Coroutine>(std::move(generator));

    _coroutines[gameObject].push_back(std::move(routine));
    return _coroutines[gameObject].back().get();
}

bool CoroutineScheduler::StopCoroutine(GameObject *gameObject, Coroutine *coroutine)
{
    if (!gameObject || !gameObject->IsActiveInHierarchy())
    {
        return false;
    }
    if (const auto it = _coroutines.find(gameObject); it != _coroutines.end())
    {
        auto &coroutineList = it->second;
        const auto coroutineIt = std::ranges::find_if(coroutineList,
                                                      [coroutine](const std::unique_ptr<Coroutine> &ptr)
                                                      {
                                                          return ptr.get() == coroutine;
                                                      });

        if (coroutineIt != coroutineList.end())
        {
            coroutineList.erase(coroutineIt);
            return true;
        }
    }
    return false;
}

void CoroutineScheduler::StopAllCoroutines(GameObject *gameObject)
{
    if (const auto it = _coroutines.find(gameObject); it != _coroutines.end())
    {
        it->second.clear();
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
    return _coroutines.erase(gameObject) > 0;
}

bool CoroutineScheduler::AdvanceCoroutine(Coroutine *coroutine)
{
    return !coroutine->MoveNext(); // Returns true when coroutine is complete
}

void CoroutineScheduler::CleanupCompleted(const std::vector<std::pair<GameObject*, Coroutine*>> &coroutinesToRemove)
{
    for (const auto &[gameObject, coroutine] : coroutinesToRemove)
    {
        if (auto it = _coroutines.find(gameObject); it != _coroutines.end())
        {
            auto &coroutineList = it->second;
            auto coroutineIt = std::ranges::find_if(coroutineList,
                                                    [coroutine](const std::unique_ptr<Coroutine> &ptr)
                                                    {
                                                        return ptr.get() == coroutine;
                                                    });

            if (coroutineIt != coroutineList.end())
            {
                coroutineList.erase(coroutineIt);

                // Remove GameObject if it has no more coroutines
                if (coroutineList.empty())
                {
                    _coroutines.erase(it);
                }
            }
        }
    }
}

void CoroutineScheduler::CleanupInvalid()
{
    for (auto it = _coroutines.begin(); it != _coroutines.end();)
    {
        const GameObject *curGameObject = it->first;
        const bool curGameObjectIsInvalid = curGameObject == nullptr || curGameObject->IsActiveInHierarchy() ||
            curGameObject->IsDestroyed();

        if (curGameObjectIsInvalid || it->second.empty())
        {
            it = _coroutines.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
