#pragma once

#include <cstdint>
#include <map>
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
            // One entry per object that has coroutines, keyed by a sequence number taken when the object's
            // first coroutine starts, so Update runs objects in that (creation) order and never in pointer
            // order. Within an object coroutines run in start order (the vector). std::map: ordered
            // iteration, O(log n) insert/erase, and nothing is ever iterated in hash order.
            struct Entry
            {
                GameObject *gameObject;
                std::vector<std::unique_ptr<Coroutine>> coroutines;
            };
            std::map<std::uint64_t, Entry> _coroutines;
            // Lookup only (never iterated): object -> its key in _coroutines. O(1) average
            std::unordered_map<GameObject*, std::uint64_t> _sequenceOf;
            std::uint64_t _nextSequence = 0;
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
            /// The coroutines of an object, or nullptr if it has none
            [[nodiscard]] const std::vector<std::unique_ptr<Coroutine>>* Find(GameObject *gameObject) const;
            /// Removes stopped/completed coroutines and objects with none left
            void RemoveFinished();
        };
    }
}
