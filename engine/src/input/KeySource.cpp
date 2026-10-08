#include "engine/input/KeySource.hpp"

#include <utility>

namespace N2Engine::Input
{
    namespace
    {
        const KeySource *g_source = nullptr;
    }

    const KeySource *KeySource::Set(const KeySource *source)
    {
        return std::exchange(g_source, source);
    }

    const KeySource *KeySource::Get()
    {
        return g_source;
    }

    void InjectedKeys::SetKey(const Key key, const bool down)
    {
        if (down)
        {
            _keys.insert(key);
        }
        else
        {
            _keys.erase(key);
        }
    }

    void InjectedKeys::SetMouseButton(const MouseButton button, const bool down)
    {
        if (down)
        {
            _buttons.insert(button);
        }
        else
        {
            _buttons.erase(button);
        }
    }

    void InjectedKeys::ReleaseAll()
    {
        _keys.clear();
        _buttons.clear();
    }

    bool InjectedKeys::IsKeyDown(const Key key) const
    {
        return _keys.contains(key);
    }

    bool InjectedKeys::IsMouseButtonDown(const MouseButton button) const
    {
        return _buttons.contains(button);
    }
}
