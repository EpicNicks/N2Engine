#pragma once

#include <string>

#include <math/Vector2.hpp>

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    /**
     * Where a UI element sits inside its parent's rect, as in Unity's RectTransform. UI objects use it instead
     * of a Positionable. All values are in canvas space (pixels, y up; see Rect).
     *
     * - anchorMin / anchorMax: corners of the anchor box, as fractions of the parent rect ((0, 0) its bottom-left,
     *   (1, 1) its top-right). Equal anchors pin the element to a point; different ones stretch it with the parent.
     * - pivot: the point of the element that anchoredPosition places, as a fraction of its own size.
     * - anchoredPosition: the pivot's offset from the reference point, the point at `pivot` inside the anchor box.
     * - sizeDelta: the element's size minus the anchor box's size (with equal anchors, simply its size).
     *
     * The rect is resolved by the UI layout (UISystem), from the canvas down, once per hit test and once per UI
     * pass; GetRect returns the last result. No rotation or scale.
     */
    class RectTransform final : public SerializableComponent
    {
    public:
        explicit RectTransform(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "RectTransform"; }

        [[nodiscard]] const Math::Vector2& GetAnchorMin() const { return _anchorMin; }
        void SetAnchorMin(const Math::Vector2 &value) { _anchorMin = value; }
        [[nodiscard]] const Math::Vector2& GetAnchorMax() const { return _anchorMax; }
        void SetAnchorMax(const Math::Vector2 &value) { _anchorMax = value; }
        [[nodiscard]] const Math::Vector2& GetPivot() const { return _pivot; }
        void SetPivot(const Math::Vector2 &value) { _pivot = value; }
        [[nodiscard]] const Math::Vector2& GetAnchoredPosition() const { return _anchoredPosition; }
        void SetAnchoredPosition(const Math::Vector2 &value) { _anchoredPosition = value; }
        [[nodiscard]] const Math::Vector2& GetSizeDelta() const { return _sizeDelta; }
        void SetSizeDelta(const Math::Vector2 &value) { _sizeDelta = value; }

        /// The bottom-left corner's offset from the anchor box's bottom-left (Unity's offsetMin)
        [[nodiscard]] Math::Vector2 GetOffsetMin() const;
        /// Moves the bottom-left corner, keeping the top-right where it is
        void SetOffsetMin(const Math::Vector2 &value);
        /// The top-right corner's offset from the anchor box's top-right (Unity's offsetMax)
        [[nodiscard]] Math::Vector2 GetOffsetMax() const;
        /// Moves the top-right corner, keeping the bottom-left where it is
        void SetOffsetMax(const Math::Vector2 &value);

        /// Anchors (0, 0)-(1, 1) with zero offsets: the element covers its parent exactly
        void StretchToParent();

        /// This element's rect inside the given parent rect (pure: GetRect is not changed)
        [[nodiscard]] Rect ResolveIn(const Rect &parent) const;
        /// The layout formula, for any values
        [[nodiscard]] static Rect Resolve(const Math::Vector2 &anchorMin, const Math::Vector2 &anchorMax,
                                          const Math::Vector2 &pivot, const Math::Vector2 &anchoredPosition,
                                          const Math::Vector2 &sizeDelta, const Rect &parent);

        /// The rect the last layout resolved (zero before any). An inactive object's is not updated.
        [[nodiscard]] const Rect& GetRect() const { return _rect; }
        /// Called by the layout
        void SetResolvedRect(const Rect &rect) { _rect = rect; }

        /// One per object: AddComponent returns the existing one
        static constexpr bool IsSingleton = true;

    private:
        // Unity's defaults for a new UI element: a 100x100 box centred on the parent's centre
        Math::Vector2 _anchorMin{0.5f, 0.5f};
        Math::Vector2 _anchorMax{0.5f, 0.5f};
        Math::Vector2 _pivot{0.5f, 0.5f};
        Math::Vector2 _anchoredPosition{0.0f, 0.0f};
        Math::Vector2 _sizeDelta{100.0f, 100.0f};
        Rect _rect{};
    };
}
