#pragma once

#include <vector>
#include <functional>
#include <algorithm>

namespace N2Engine
{
    namespace Base
    {
        template <typename... ARGS>
        class EventHandler
        {
        private:
            struct Subscriber
            {
                size_t id;
                std::function<void(ARGS...)> func;
            };

            std::vector<Subscriber> _subscribers;
            size_t _nextId = 0;

        public:
            size_t operator+=(const std::function<void(ARGS...)> &func)
            {
                _subscribers.push_back({_nextId, func});
                return _nextId++;
            }

            void operator-=(size_t id)
            {
                _subscribers.erase(
                    std::remove_if(_subscribers.begin(), _subscribers.end(),
                                   [id](auto &sub)
                                   { return sub.id == id; }),
                    _subscribers.end());
            }

            void operator()(ARGS... args)
            {
                for (auto &sub : _subscribers)
                    sub.func(args...);
            }

            size_t GetSubscriberCount() const
            {
                return _subscribers.size();
            }
        };

    }
}