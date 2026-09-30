#pragma once

#include <vector>
#include <functional>
#include <algorithm>

namespace N2Engine::Base
{
    template <typename... ARGS>
    class EventHandler
    {
    private:
        struct Subscriber
        {
            size_t id;
            std::function<void(ARGS...)> func;
            bool removed = false;
        };

        std::vector<Subscriber> _subscribers;
        size_t _nextId = 0;
        int _dispatchDepth = 0;

        void CompactIfIdle()
        {
            if (_dispatchDepth == 0)
            {
                std::erase_if(_subscribers, [](const Subscriber &sub) { return sub.removed; });
            }
        }

    public:
        /// Safe during dispatch: the new subscriber is first called on the next dispatch
        size_t operator+=(const std::function<void(ARGS...)> &func)
        {
            _subscribers.push_back({_nextId, func});
            return _nextId++;
        }

        /// Safe during dispatch (including a handler removing itself): the subscriber isn't called
        /// again, and is erased once the outermost dispatch finishes
        void operator-=(size_t id)
        {
            for (auto &sub : _subscribers)
            {
                if (sub.id == id)
                {
                    sub.removed = true;
                }
            }
            CompactIfIdle();
        }

        void operator()(ARGS... args)
        {
            // By index over the subscribers present at the start: handlers may subscribe (appending,
            // possibly reallocating) or unsubscribe (flagging) while this runs
            struct DispatchScope
            {
                EventHandler &handler;
                explicit DispatchScope(EventHandler &h) : handler(h) { ++handler._dispatchDepth; }
                ~DispatchScope()
                {
                    --handler._dispatchDepth;
                    handler.CompactIfIdle();
                }
            } scope{*this};

            const size_t count = _subscribers.size();
            for (size_t i = 0; i < count; ++i)
            {
                if (_subscribers[i].removed)
                {
                    continue;
                }
                // A copy: a handler that subscribes can reallocate the vector under the call
                const auto func = _subscribers[i].func;
                func(args...);
            }
        }

        size_t GetSubscriberCount() const
        {
            return static_cast<size_t>(std::ranges::count_if(_subscribers, [](const Subscriber &sub) { return !sub.removed; }));
        }
    };
}
