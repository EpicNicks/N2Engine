#pragma once

#include <cstdint>
#include <type_traits>
#include <new>
#include <cstddef>

namespace N2Engine
{
    namespace Scheduling
    {
        class ICoroutineWait
        {
        private:
            static constexpr size_t STORAGE_SIZE = 32;
            static constexpr size_t STORAGE_ALIGN = alignof(std::max_align_t);

            alignas(STORAGE_ALIGN) char _storage[STORAGE_SIZE];
            bool (*_wait_fn)(const void *);
            void (*_destroy_fn)(void *);
            void (*_copy_fn)(void *, const void *);
            void (*_move_fn)(void *, void *);

        public:
            // Template constructor for type erasure
            template <typename T>
            ICoroutineWait(T &&wait_obj)
            {
                using DecayedT = std::decay_t<T>;
                static_assert(sizeof(DecayedT) <= STORAGE_SIZE,
                              "Wait object too large for fixed storage");
                static_assert(alignof(DecayedT) <= STORAGE_ALIGN,
                              "Wait object alignment too strict");

                // Construct object in storage
                new (_storage) DecayedT(std::forward<T>(wait_obj));

                // Set up function pointers
                _wait_fn = [](const void *ptr) -> bool
                {
                    return static_cast<const DecayedT *>(ptr)->Wait();
                };

                _destroy_fn = [](void *ptr)
                {
                    static_cast<DecayedT *>(ptr)->~DecayedT();
                };

                _copy_fn = [](void *dst, const void *src)
                {
                    new (dst) DecayedT(*static_cast<const DecayedT *>(src));
                };

                _move_fn = [](void *dst, void *src)
                {
                    new (dst) DecayedT(std::move(*static_cast<DecayedT *>(src)));
                };
            }

            // Copy constructor
            ICoroutineWait(const ICoroutineWait &other)
                : _wait_fn(other._wait_fn), _destroy_fn(other._destroy_fn), _copy_fn(other._copy_fn), _move_fn(other._move_fn)
            {
                _copy_fn(_storage, other._storage);
            }

            // Move constructor
            ICoroutineWait(ICoroutineWait &&other) noexcept
                : _wait_fn(other._wait_fn), _destroy_fn(other._destroy_fn), _copy_fn(other._copy_fn), _move_fn(other._move_fn)
            {
                _move_fn(_storage, other._storage);
            }

            // Assignment operators
            ICoroutineWait &operator=(const ICoroutineWait &other)
            {
                if (this != &other)
                {
                    _destroy_fn(_storage);
                    _wait_fn = other._wait_fn;
                    _destroy_fn = other._destroy_fn;
                    _copy_fn = other._copy_fn;
                    _move_fn = other._move_fn;
                    _copy_fn(_storage, other._storage);
                }
                return *this;
            }

            ICoroutineWait &operator=(ICoroutineWait &&other) noexcept
            {
                if (this != &other)
                {
                    _destroy_fn(_storage);
                    _wait_fn = other._wait_fn;
                    _destroy_fn = other._destroy_fn;
                    _copy_fn = other._copy_fn;
                    _move_fn = other._move_fn;
                    _move_fn(_storage, other._storage);
                }
                return *this;
            }

            // Destructor
            ~ICoroutineWait()
            {
                _destroy_fn(_storage);
            }

            // Main interface
            bool Wait()
            {
                return _wait_fn(_storage);
            }
        };

        // Your concrete wait classes - no inheritance needed!
        class WaitForNextFrame
        {
        public:
            bool Wait();
        };

        class WaitForSeconds
        {
        private:
            float _waitSeconds{};
            mutable float _elapsedSeconds{}; // mutable since Wait() is const

        public:
            explicit WaitForSeconds(float seconds) : _waitSeconds{seconds} {}
            bool Wait();
        };

        class WaitForever
        {
        public:
            bool Wait();
        };
    }
}