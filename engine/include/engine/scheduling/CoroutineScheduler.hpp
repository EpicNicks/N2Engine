#pragma once
#include <unordered_map>
#include <vector>
#include <memory>
#include "engine/scheduling/Coroutine.hpp"

namespace N2Engine
{
    class GameObject;

    namespace Scheduling
    {
        class CoroutineScheduler
        {
        private:
            static std::unordered_map<GameObject *, std::vector<std::unique_ptr<Coroutine>>> coroutines;

        public:
            static void Update();

            static Coroutine *StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator);
            static bool StopCoroutine(GameObject *gameObject, Coroutine *coroutine);
            static void StopAllCoroutines(GameObject *gameObject);
            static bool RemoveGameObject(GameObject *gameObject);

        private:
            static bool AdvanceCoroutine(Coroutine *coroutine);
            static void CleanupCompleted(const std::vector<std::pair<GameObject *, Coroutine *>> &coroutinesToRemove);
        };
    }
}