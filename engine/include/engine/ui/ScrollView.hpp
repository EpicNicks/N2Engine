#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <math/Vector2.hpp>

#include "engine/base/EventHandler.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/ui/Rect.hpp"

namespace N2Engine::UI
{
    class RectTransform;

    /// What a ScrollView does at the edges of its content (Unity's ScrollRect.MovementType)
    enum class ScrollMovementType : std::uint8_t
    {
        /// The content can be moved anywhere
        Unrestricted,
        /// It can be pulled past an edge, with growing resistance, and springs back when let go (the default)
        Elastic,
        /// It stops at the edges
        Clamped
    };

    N2_SERIALIZE_ENUM(ScrollMovementType, {
                      { ScrollMovementType::Unrestricted, "Unrestricted" },
                      { ScrollMovementType::Elastic, "Elastic" },
                      { ScrollMovementType::Clamped, "Clamped" }
                      })

    /**
     * A scrollable area (Unity's ScrollRect), issue #445. Its object's rect is the viewport; the content is a child
     * RectTransform (the `content` reference, or while none is set the first child with a RectTransform) that it moves by
     * the content's anchoredPosition. Put a RectMask on the same object to clip the content to the viewport (what
     * UISystem::CreateScrollView builds).
     *
     * Input, read in OnUpdate from the application's mouse and PointerDispatcher (the web player's first finger drives the
     * mouse, so touch works the same way):
     * - the mouse wheel while the pointer is over the view or anything below it: scrollSensitivity canvas units a notch;
     *   wheel up shows content further up. With only one axis enabled the larger wheel axis drives it.
     * - a drag that starts on the view or anything below it (a button in the content too), once the pointer has moved
     *   dragThreshold canvas units from the press: the content follows the pointer. A Button under the press then loses
     *   the press (Button::CancelPress), so a drag never clicks. Dragging is for screen-space overlay canvases (a
     *   world-space canvas still takes the wheel).
     * - dragging a scrollbar handle (verticalScrollbar / horizontalScrollbar) moves the view across its track.
     * The input path calls the public methods below (Scroll, BeginDrag, Drag, EndDrag, Step), which tests and scripts can
     * call themselves.
     *
     * Positions come from the last layout (RectTransform::GetRect of the view and the content); a move updates the
     * content's resolved rect at once, so reading the position back right after setting it works. Content smaller than
     * the view on an axis sits at its top (vertical) or left (horizontal) edge when it is held in.
     *
     * The normalized position (Unity's) is, per axis, 0 when the content's left (bottom) edge is at the view's left
     * (bottom) edge and 1 when its right (top) edge is at the view's right (top) edge: vertical 1 is the top. Content no
     * bigger than the view reads horizontal 0 and vertical 1.
     *
     * Not part of any simulation: it uses unscaled time, and the UI is outside the network simulation.
     */
    class ScrollView final : public SerializableComponent
    {
    public:
        using ListenerId = std::size_t;

        static constexpr float DefaultScrollSensitivity = 30.0f;
        static constexpr float DefaultElasticity = 0.1f;
        static constexpr float DefaultDecelerationRate = 0.135f;
        static constexpr float DefaultDragThreshold = 10.0f;

        explicit ScrollView(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "ScrollView"; }

        // ===== Settings (serialized) =====
        /// The content set (nullptr: none set, the first child with a RectTransform is used)
        [[nodiscard]] RectTransform *GetContentSetting() const { return _content; }
        void SetContent(RectTransform *content) { _content = content; }
        /// The content used now: the one set, else the first child with a RectTransform, else nullptr
        [[nodiscard]] RectTransform *GetContent() const;
        [[nodiscard]] bool GetHorizontal() const { return _horizontal; }
        void SetHorizontal(bool horizontal) { _horizontal = horizontal; }
        [[nodiscard]] bool GetVertical() const { return _vertical; }
        void SetVertical(bool vertical) { _vertical = vertical; }
        [[nodiscard]] ScrollMovementType GetMovementType() const { return _movementType; }
        void SetMovementType(ScrollMovementType type) { _movementType = type; }
        /// Seconds an elastic view takes to spring back (Unity's elasticity)
        [[nodiscard]] float GetElasticity() const { return _elasticity; }
        void SetElasticity(float seconds) { _elasticity = seconds; }
        /// Whether the content keeps moving after a drag, slowing down
        [[nodiscard]] bool GetInertia() const { return _inertia; }
        void SetInertia(bool inertia) { _inertia = inertia; }
        /// The share of the speed left after one second of coasting (0 stops at once, 1 never slows)
        [[nodiscard]] float GetDecelerationRate() const { return _decelerationRate; }
        void SetDecelerationRate(float rate) { _decelerationRate = rate; }
        /// Canvas units per wheel notch
        [[nodiscard]] float GetScrollSensitivity() const { return _scrollSensitivity; }
        void SetScrollSensitivity(float sensitivity) { _scrollSensitivity = sensitivity; }
        /// Canvas units the pointer must move from the press before a drag starts
        [[nodiscard]] float GetDragThreshold() const { return _dragThreshold; }
        void SetDragThreshold(float threshold) { _dragThreshold = threshold; }
        /// Optional scrollbar handles: placed and sized inside their parent's rect (the track) by the scroll position, and
        /// draggable. nullptr: none.
        [[nodiscard]] RectTransform *GetVerticalScrollbar() const { return _verticalScrollbar; }
        void SetVerticalScrollbar(RectTransform *handle) { _verticalScrollbar = handle; }
        [[nodiscard]] RectTransform *GetHorizontalScrollbar() const { return _horizontalScrollbar; }
        void SetHorizontalScrollbar(RectTransform *handle) { _horizontalScrollbar = handle; }

        // ===== Position =====
        /// (horizontal, vertical), each 0..1 inside the content (outside while an elastic view is pulled past an edge)
        [[nodiscard]] Math::Vector2 GetNormalizedPosition() const;
        /// Moves the content to the normalized position (each axis, enabled or not; clamped to 0..1 unless Unrestricted)
        /// and stops any movement
        void SetNormalizedPosition(const Math::Vector2 &position);
        void SetHorizontalNormalizedPosition(float position);
        void SetVerticalNormalizedPosition(float position);
        /// Canvas units per second the content is moving at (coasting, or springing back)
        [[nodiscard]] const Math::Vector2 &GetVelocity() const { return _velocity; }
        void SetVelocity(const Math::Vector2 &velocity) { _velocity = velocity; }
        /// Stops coasting and springing back
        void StopMovement() { _velocity = Math::Vector2{0.0f, 0.0f}; }

        /// The view's and the content's rects from the last layout (zero before one)
        [[nodiscard]] Rect GetViewRect() const;
        [[nodiscard]] Rect GetContentRect() const;

        // ===== Input (what OnUpdate calls; public for tests and scripts) =====
        /// A wheel step: `wheel` in notches, +y up (Mouse::GetScrollDelta)
        void Scroll(const Math::Vector2 &wheel);
        /// Starts a drag with the pointer at `pointer` (canvas space)
        void BeginDrag(const Math::Vector2 &pointer);
        /// The pointer, still held, is at `pointer`: the content follows it (with resistance past an edge when Elastic)
        void Drag(const Math::Vector2 &pointer);
        void EndDrag();
        [[nodiscard]] bool IsDragging() const { return _dragging; }
        /// One frame of movement, `deltaTime` seconds: coasting, springing back, clamping; also places the scrollbars
        void Step(float deltaTime);

        // ===== OnValueChanged =====
        /// fn(normalizedPosition) whenever the content moves. Guarded like Button's listeners: one that throws is logged.
        ListenerId AddOnValueChanged(std::function<void(Math::Vector2)> listener);
        void RemoveOnValueChanged(ListenerId id);
        void ClearOnValueChanged();
        [[nodiscard]] std::size_t GetOnValueChangedListenerCount() const;

        void OnUpdate() override;
        void OnDisable() override;

        /// The rubber-band distance of Unity's ScrollRect: how far content pulled `overStretching` past an edge shows,
        /// for a view `viewSize` long. Always less than viewSize.
        [[nodiscard]] static float RubberDelta(float overStretching, float viewSize);
        /// Unity's Mathf.SmoothDamp without a speed limit: from `current` towards `target`, `velocity` updated
        [[nodiscard]] static float SmoothDamp(float current, float target, float &velocity, float smoothTime, float deltaTime);

        static constexpr bool IsSingleton = true;

    private:
        // Serialized
        RectTransform *_content = nullptr;
        bool _horizontal = false;
        bool _vertical = true;
        ScrollMovementType _movementType = ScrollMovementType::Elastic;
        float _elasticity = DefaultElasticity;
        bool _inertia = true;
        float _decelerationRate = DefaultDecelerationRate;
        float _scrollSensitivity = DefaultScrollSensitivity;
        float _dragThreshold = DefaultDragThreshold;
        RectTransform *_verticalScrollbar = nullptr;
        RectTransform *_horizontalScrollbar = nullptr;

        // Runtime
        Math::Vector2 _velocity{0.0f, 0.0f};
        bool _dragging = false;
        Math::Vector2 _dragStartPointer{0.0f, 0.0f};
        Math::Vector2 _dragStartContent{0.0f, 0.0f};
        Math::Vector2 _previousPosition{0.0f, 0.0f};
        bool _hasPrevious = false;
        // The pointer path in OnUpdate
        bool _pressTracked = false;      // the left button went down over this view
        Math::Vector2 _pressPointer{0.0f, 0.0f};
        RectTransform *_draggedHandle = nullptr; // a scrollbar handle being dragged
        Math::Vector2 _handleStartPointer{0.0f, 0.0f};
        Math::Vector2 _handleStartPosition{0.0f, 0.0f};
        bool _pendingNotify = false; // the content moved since the listeners were last called
        bool _deferNotify = false;   // inside OnUpdate: the listeners are called once, at its end

        std::shared_ptr<Base::EventHandler<Math::Vector2>> _onValueChanged =
            std::make_shared<Base::EventHandler<Math::Vector2>>();

        /// How far the content must move (canvas units) to be held in, if it moved by `delta` first; 0 on an axis that
        /// is disabled or Unrestricted
        [[nodiscard]] Math::Vector2 CalculateOffset(const Math::Vector2 &delta) const;
        /// Sets the content's anchoredPosition (the enabled axes only) and moves its resolved rect with it
        void SetContentPosition(const Math::Vector2 &position, bool allAxes = false);
        /// Calls the listeners if the normalized position changed since they were last called
        void NotifyIfMoved();
        /// Places and sizes the scrollbar handles for the current position
        void UpdateScrollbars();
        /// The pointer path of OnUpdate (the application's mouse and dispatcher)
        void ProcessPointer();
    };
}
