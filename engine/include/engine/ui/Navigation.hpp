#pragma once

#include <cstdint>
#include <string>

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::UI
{
    class RectTransform;

    /// A direction of keyboard or gamepad navigation, on screen
    enum class NavigationDirection : std::uint8_t
    {
        Up,
        Down,
        Left,
        Right
    };

    /// How the selection moves away from an element (Unity's Navigation.Mode)
    enum class NavigationMode : std::uint8_t
    {
        /// To the nearest selectable element in the direction pressed (what an element without a Navigation component does)
        Automatic,
        /// To the element set for that direction, if any
        Explicit,
        /// Nowhere, and the arrows and Tab never come to it; a script or a click still selects it
        None
    };

    N2_SERIALIZE_ENUM(NavigationMode, {
                      { NavigationMode::Automatic, "Automatic" },
                      { NavigationMode::Explicit, "Explicit" },
                      { NavigationMode::None, "None" }
                      })

    /**
     * Sets how keyboard and gamepad navigation moves from and to a UI element (issue #445; docs/ui.html#navigation). Only
     * needed to change the default: a Button or Slider without one navigates automatically. With Explicit, the element
     * chosen for each direction is the object of the RectTransform set for it; a direction with none set goes nowhere.
     */
    class Navigation final : public SerializableComponent
    {
    public:
        explicit Navigation(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Navigation"; }

        [[nodiscard]] NavigationMode GetMode() const { return _mode; }
        void SetMode(const NavigationMode mode) { _mode = mode; }
        /// The element the direction goes to with Explicit (nullptr: nowhere)
        [[nodiscard]] RectTransform *GetNeighbor(NavigationDirection direction) const;
        void SetNeighbor(NavigationDirection direction, RectTransform *neighbor);

        static constexpr bool IsSingleton = true;

    private:
        NavigationMode _mode = NavigationMode::Automatic;
        RectTransform *_selectOnUp = nullptr;
        RectTransform *_selectOnDown = nullptr;
        RectTransform *_selectOnLeft = nullptr;
        RectTransform *_selectOnRight = nullptr;
    };
}
