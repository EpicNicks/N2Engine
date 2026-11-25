#pragma once

#include <array>
#include <optional>
#include <string>

#include <math/Vector2.hpp>

#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/Transform2D.hpp"

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
     * - rotation: degrees, counter-clockwise on screen, about the pivot (issue #445).
     * - scale: about the pivot, (1, 1) by default; a negative component mirrors, 0 shows nothing (issue #445).
     *
     * Rotation and scale do not move the layout: the rect (GetRect) is resolved as without them, in the parent's
     * space before any of them, and the element and everything below it is then drawn and hit through
     * GetLocalToCanvas, which turns and scales that rect about the pivot, within whatever the parents do. Layout groups
     * and content size fitters place and measure unturned rects. A canvas's own object ignores them: an overlay canvas
     * covers the viewport, and a world canvas is turned and scaled by its object's transform.
     *
     * The rect is resolved by the UI layout (UISystem), from the canvas down, once per hit test and once per UI
     * pass; GetRect returns the last result.
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
        /// Degrees, counter-clockwise on screen, about the pivot
        [[nodiscard]] float GetRotation() const { return _rotation; }
        void SetRotation(const float degrees) { _rotation = degrees; }
        /// About the pivot
        [[nodiscard]] const Math::Vector2& GetScale() const { return _scale; }
        void SetScale(const Math::Vector2 &value) { _scale = value; }

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

        /// The rect the last layout resolved (zero before any). An inactive object's is not updated. Without rotation or
        /// scale here or above it is in canvas space; with them, in the space GetLocalToCanvas maps to the canvas.
        [[nodiscard]] const Rect& GetRect() const { return _rect; }
        /// Called by the layout
        void SetResolvedRect(const Rect &rect) { _rect = rect; }

        /// This element's own rotation and scale about the pivot of `rect` (the identity without any)
        [[nodiscard]] Transform2D LocalTransform(const Rect &rect) const;
        /// From the space of GetRect to canvas space, as the last layout resolved it: this element's rotation and scale
        /// about its pivot, then each parent's (the identity when none turns or scales)
        [[nodiscard]] const Transform2D& GetLocalToCanvas() const { return _localToCanvas; }
        /// Called by the layout
        void SetLocalToCanvas(const Transform2D &transform) { _localToCanvas = transform; }
        /// A point of GetRect's space in canvas space
        [[nodiscard]] Math::Vector2 LocalToCanvas(const Math::Vector2 &point) const { return _localToCanvas.Apply(point); }
        /// A canvas point in GetRect's space; nothing when a scale of 0 flattens the element
        [[nodiscard]] std::optional<Math::Vector2> CanvasToLocal(const Math::Vector2 &point) const;
        /// Where the corners of GetRect are drawn, in canvas space: bottom-left, top-left, top-right, bottom-right of the
        /// element (Unity's GetWorldCorners order; a mirroring scale swaps where they land)
        [[nodiscard]] std::array<Math::Vector2, 4> GetCanvasCorners() const;

        /// One per object: AddComponent returns the existing one
        static constexpr bool IsSingleton = true;

    private:
        // Unity's defaults for a new UI element: a 100x100 box centred on the parent's centre
        Math::Vector2 _anchorMin{0.5f, 0.5f};
        Math::Vector2 _anchorMax{0.5f, 0.5f};
        Math::Vector2 _pivot{0.5f, 0.5f};
        Math::Vector2 _anchoredPosition{0.0f, 0.0f};
        Math::Vector2 _sizeDelta{100.0f, 100.0f};
        float _rotation = 0.0f;
        Math::Vector2 _scale{1.0f, 1.0f};
        Rect _rect{};
        Transform2D _localToCanvas{};
    };
}
