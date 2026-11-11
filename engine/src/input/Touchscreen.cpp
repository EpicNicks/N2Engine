#include "engine/input/Touchscreen.hpp"

#include <algorithm>

namespace N2Engine::Input
{
    std::string_view TouchPhaseName(const TouchPhase phase)
    {
        switch (phase)
        {
        case TouchPhase::Began: return "Began";
        case TouchPhase::Moved: return "Moved";
        case TouchPhase::Stationary: return "Stationary";
        case TouchPhase::Ended: return "Ended";
        case TouchPhase::Canceled: return "Canceled";
        }
        return "Began";
    }

    Touchscreen &Touchscreen::Get()
    {
        static Touchscreen touchscreen;
        return touchscreen;
    }

    Touchscreen::Finger *Touchscreen::Find(const int id)
    {
        for (Finger &f : _fingers)
        {
            if (f.id == id && !f.ended)
            {
                return &f;
            }
        }
        return nullptr;
    }

    void Touchscreen::Down(const int id, const Math::Vector2 &position)
    {
        if (Finger *f = Find(id))
        {
            f->position = position;
            return;
        }
        const auto live = std::ranges::count_if(_fingers, [](const Finger &f) { return !f.ended; });
        if (static_cast<std::size_t>(live) >= MaxTouches)
        {
            return;
        }
        Finger f;
        f.id = id;
        f.position = position;
        f.published = position;
        _fingers.push_back(f);
    }

    void Touchscreen::Up(const int id, const Math::Vector2 &position)
    {
        if (Finger *f = Find(id))
        {
            f->position = position;
            f->ended = true;
        }
    }

    void Touchscreen::Cancel(const int id)
    {
        if (Finger *f = Find(id))
        {
            f->ended = true;
            f->canceled = true;
        }
    }

    void Touchscreen::CancelAll()
    {
        for (Finger &f : _fingers)
        {
            if (!f.ended)
            {
                f.ended = true;
                f.canceled = true;
            }
        }
    }

    void Touchscreen::Update()
    {
        _touches.clear();
        for (auto it = _fingers.begin(); it != _fingers.end();)
        {
            Finger &f = *it;
            Touch t;
            t.id = f.id;
            t.position = f.position;
            if (f.beganPending)
            {
                // Began first, even when it was already lifted: the end follows next frame
                t.phase = TouchPhase::Began;
                f.beganPending = false;
                f.published = f.position;
                _touches.push_back(t);
                ++it;
                continue;
            }
            if (f.ended)
            {
                t.phase = f.canceled ? TouchPhase::Canceled : TouchPhase::Ended;
                _touches.push_back(t);
                it = _fingers.erase(it);
                continue;
            }
            t.phase = (f.position.x != f.published.x || f.position.y != f.published.y) ? TouchPhase::Moved
                                                                                      : TouchPhase::Stationary;
            f.published = f.position;
            _touches.push_back(t);
            ++it;
        }
    }

    void Touchscreen::Reset()
    {
        _fingers.clear();
        _touches.clear();
    }
}
