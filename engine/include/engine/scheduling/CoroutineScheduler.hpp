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
        /// Runs a scene's coroutines once per frame (Scene::AdvanceCoroutines). Coroutines belong to a
        /// GameObject and stop when it's deactivated or destroyed, like Unity's.
        class CoroutineScheduler
        {
        private:
            std::unordered_map<GameObject*, std::vector<std::unique_ptr<Coroutine>>> _coroutines;
            Scene *_scene;
            // True while Update runs coroutine bodies: stops are deferred to the end of the update so a
            // coroutine is never destroyed while its own body is executing
            bool _updating = false;

        public:
            explicit CoroutineScheduler(Scene *scene);

            void Update();

            /// @returns nullptr if the object is null or inactive
            Coroutine* StartCoroutine(GameObject *gameObject, std::generator<ICoroutineWait> &&generator);
            bool StopCoroutine(GameObject *gameObject, Coroutine *coroutine);
            void StopAllCoroutines(GameObject *gameObject);

            static Coroutine* StartCoroutine(const Scene *curScene, GameObject *gameObject,
                                             std::generator<ICoroutineWait> &&generator);
            static bool StopCoroutine(const Scene *curScene, GameObject *gameObject, Coroutine *coroutine);
            static void StopAllCoroutines(const Scene *curScene, GameObject *gameObject);

            bool RemoveGameObject(GameObject *gameObject);

        private:
            /// @returns false once the coroutine is finished (completed, stopped or failed)
            static bool AdvanceCoroutine(Coroutine *coroutine);
            [[nodiscard]] bool Contains(GameObject *gameObject, const Coroutine *coroutine) const;
            /// Removes stopped/completed coroutines and objects with none left
            void RemoveFinished();
        };
    }
}
