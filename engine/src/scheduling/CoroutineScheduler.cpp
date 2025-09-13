#include "engine/scheduling/CoroutineScheduler.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/GameObject.hpp"

#include <algorithm>
#include <generator>

using namespace N2Engine::Scheduling;

void CoroutineScheduler::Update()
{
    std::vector<std::pair<GameObject *, Coroutine *>> coroutinesToRemove;

    for (auto &[gameObject, coroutineList] : coroutines)
    {
        for (auto &coroutine : coroutineList)
        {
            bool isComplete = AdvanceCoroutine(coroutine.get());
            if (isComplete)
            {
                coroutinesToRemove.emplace_back(gameObject, coroutine.get());
            }
        }
    }
    CleanupCompleted(coroutinesToRemove);
}

Coroutine *CoroutineScheduler::StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator)
{
    auto routine = std::make_unique<Coroutine>(std::move(generator));
    Coroutine *rawPtr = routine.get(); // Store raw pointer to return

    auto it = coroutines.find(gameObject);
    if (it != coroutines.end())
    {
        it->second.push_back(std::move(routine));
    }
    else
    {
        std::vector<std::unique_ptr<Coroutine>> newList;
        newList.push_back(std::move(routine));
        coroutines[gameObject] = std::move(newList);
    }
    return rawPtr;
}

bool CoroutineScheduler::StopCoroutine(GameObject *gameObject, Coroutine *coroutine)
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
            return true;
        }
    }
    return false;
}

void CoroutineScheduler::StopAllCoroutines(GameObject *gameObject)
{
    auto it = coroutines.find(gameObject);
    if (it != coroutines.end())
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