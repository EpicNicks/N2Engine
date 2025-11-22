#include <algorithm>
#include <generator>

#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/GameObject.hpp"

using namespace N2Engine::Scheduling;

std::unordered_map<N2Engine::GameObject *, std::vector<std::unique_ptr<Coroutine>>> CoroutineScheduler::coroutines;

void CoroutineScheduler::Update()
{
    std::vector<std::pair<GameObject *, Coroutine *>> coroutinesToRemove;
    CleanupInvalid();

    for (auto &[gameObject, coroutineList] : coroutines)
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

Coroutine *CoroutineScheduler::StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator)
{
    if (!gameObject || !gameObject->IsActiveInHierarchy())
    {
        return nullptr;
    }
    auto routine = std::make_unique<Coroutine>(std::move(generator));

    coroutines[gameObject].push_back(std::move(routine));
    return coroutines[gameObject].back().get();
}

bool CoroutineScheduler::StopCoroutine(GameObject *gameObject, Coroutine *coroutine)
{
    if (!gameObject || !gameObject->IsActiveInHierarchy())
    {
        return false;
    }
    if (const auto it = coroutines.find(gameObject); it != coroutines.end())
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
    if (const auto it = coroutines.find(gameObject); it != coroutines.end())
    {
        it->second.clear();
    }
}

bool CoroutineScheduler::RemoveGameObject(GameObject *gameObject)
{
    return coroutines.erase(gameObject) > 0;
}

bool CoroutineScheduler::AdvanceCoroutine(Coroutine *coroutine)
{
    return !coroutine->MoveNext(); // Returns true when coroutine is complete
}

void CoroutineScheduler::CleanupCompleted(const std::vector<std::pair<GameObject *, Coroutine *>> &coroutinesToRemove)
{
    for (const auto &[gameObject, coroutine] : coroutinesToRemove)
    {
        auto it = coroutines.find(gameObject);
        if (it != coroutines.end())
        {
            auto &coroutineList = it->second;
            auto coroutineIt = std::find_if(coroutineList.begin(), coroutineList.end(),
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
                    coroutines.erase(it);
                }
            }
        }
    }
}

void CoroutineScheduler::CleanupInvalid()
{
    for (auto it = coroutines.begin(); it != coroutines.end();)
    {
        GameObject *gameObject = it->first;

        if (gameObject == nullptr || !gameObject->IsActiveInHierarchy() || gameObject->IsDestroyed())
        {
            it = coroutines.erase(it);
        }
        else if (it->second.empty())
        {
            it = coroutines.erase(it);
        }
        else
        {
            ++it;
        }
    }
}