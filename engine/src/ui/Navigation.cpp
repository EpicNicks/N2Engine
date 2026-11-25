#include "engine/ui/Navigation.hpp"

#include "engine/ui/RectTransform.hpp"

namespace N2Engine::UI
{
    Navigation::Navigation(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterMember("mode", _mode);
        RegisterComponentRef("selectOnUp", _selectOnUp);
        RegisterComponentRef("selectOnDown", _selectOnDown);
        RegisterComponentRef("selectOnLeft", _selectOnLeft);
        RegisterComponentRef("selectOnRight", _selectOnRight);
    }

    RectTransform *Navigation::GetNeighbor(const NavigationDirection direction) const
    {
        switch (direction)
        {
        case NavigationDirection::Up:
            return _selectOnUp;
        case NavigationDirection::Down:
            return _selectOnDown;
        case NavigationDirection::Left:
            return _selectOnLeft;
        case NavigationDirection::Right:
            return _selectOnRight;
        }
        return nullptr;
    }

    void Navigation::SetNeighbor(const NavigationDirection direction, RectTransform *neighbor)
    {
        switch (direction)
        {
        case NavigationDirection::Up:
            _selectOnUp = neighbor;
            break;
        case NavigationDirection::Down:
            _selectOnDown = neighbor;
            break;
        case NavigationDirection::Left:
            _selectOnLeft = neighbor;
            break;
        case NavigationDirection::Right:
            _selectOnRight = neighbor;
            break;
        }
    }
}
