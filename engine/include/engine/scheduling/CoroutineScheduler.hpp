#pragma once
#include <unordered_map>
#include <vector>
#include <memory>
#include "engine/scheduling/Coroutine.hpp"

namespace N2Engine
{
    class Scene;
    class GameObject;

    namespace Scheduling
    {
        class CoroutineScheduler
        {
        private:
            std::unordered_map<GameObject*, std::vector<std::unique_ptr<Coroutine>>> _coroutines;
            Scene *_scene;

        public:
            explicit CoroutineScheduler(Scene *scene);
            void Update();

            Coroutine* StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator);
            bool StopCoroutine(GameObject *gameObject, Coroutine *coroutine);
            void StopAllCoroutines(GameObject *gameObject);

            static Coroutine* StartCoroutine(const Scene *curScene, GameObject *gameObject,
                                             std::generator<ICoroutineWait> &&generator);
            static bool StopCoroutine(const Scene *curScene, GameObject *gameObject, Coroutine *coroutine);
            static void StopAllCoroutines(const Scene *curScene, GameObject *gameObject);
            bool RemoveGameObject(GameObject *gameObject);

        private:
            static bool AdvanceCoroutine(Coroutine *coroutine);
            void CleanupCompleted(const std::vector<std::pair<GameObject*, Coroutine*>> &coroutinesToRemove);
            void CleanupInvalid();
        };
    }
}
