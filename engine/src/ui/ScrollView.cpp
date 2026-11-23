#include "engine/ui/ScrollView.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::UI
{
    namespace
    {
        /// Whether `object` is `ancestor` or below it in the hierarchy
        bool IsSelfOrBelow(const GameObject *object, const GameObject &ancestor)
        {
            while (object)
            {
                if (object == &ancestor)
                {
                    return true;
                }
                const GameObject::Ptr parent = object->GetParent();
                object = parent.get(); // the scene owns it, so it outlives this walk
            }
            return false;
        }

        /// The root canvas above (or on) an object, or nullptr
        const Canvas *RootCanvasOf(const GameObject &gameObject)
        {
            const Canvas *found = nullptr;
            for (const GameObject *object = &gameObject; object;)
            {
                for (const Canvas *canvas : object->GetComponents<Canvas>())
                {
                    if (canvas && canvas->IsRootCanvas())
                    {
                        found = canvas;
                    }
                }
                const GameObject::Ptr parent = object->GetParent();
                object = parent.get();
            }
            return found;
        }

        float Clamp01(const float value) { return std::clamp(value, 0.0f, 1.0f); }

        /// The normalized position on one axis: where the view's start is across the hidden part of the content
        float NormalizedOnAxis(const float viewMin, const float viewSize, const float contentMin, const float contentSize,
                               const float whenSmaller)
        {
            const float hidden = contentSize - viewSize;
            if (!(hidden > 0.0f))
            {
                return whenSmaller;
            }
            return (viewMin - contentMin) / hidden;
        }

        /// How far the content must move on one axis to be held in the view (`pinToMax`: content smaller than the view
        /// sits at the view's top edge, else at its left edge)
        float OffsetOnAxis(const float viewMin, const float viewMax, const float contentMin, const float contentMax,
                           const bool pinToMax)
        {
            if (contentMax - contentMin <= viewMax - viewMin)
            {
                return pinToMax ? viewMax - contentMax : viewMin - contentMin;
            }
            if (contentMin > viewMin)
            {
                return viewMin - contentMin;
            }
            if (contentMax < viewMax)
            {
                return viewMax - contentMax;
            }
            return 0.0f;
        }

        /// Offsets below this (canvas units) count as none, so a spring back ends exactly at the edge
        constexpr float SettleDistance = 0.01f;
        /// Speeds below this (canvas units per second) count as stopped, as in Unity
        constexpr float StopSpeed = 1.0f;
    }

    ScrollView::ScrollView(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        RegisterComponentRef("content", _content).Tooltip("The moved child; none: the first child with a RectTransform");
        RegisterMember("horizontal", _horizontal);
        RegisterMember("vertical", _vertical);
        RegisterMember("movementType", _movementType);
        RegisterMember("elasticity", _elasticity).Range(0.0, 10.0).Tooltip("Seconds to spring back").VisibleWhen("movementType", {"Elastic"});
        RegisterMember("inertia", _inertia);
        RegisterMember("decelerationRate", _decelerationRate).Range(0.0, 1.0).VisibleWhen("inertia", {true});
        RegisterMember("scrollSensitivity", _scrollSensitivity).Tooltip("Canvas units per wheel notch");
        RegisterMember("dragThreshold", _dragThreshold).Range(0.0, 1000.0).Tooltip("Canvas units before a drag starts");
        RegisterComponentRef("verticalScrollbar", _verticalScrollbar).Tooltip("A handle placed inside its parent's rect");
        RegisterComponentRef("horizontalScrollbar", _horizontalScrollbar).Tooltip("A handle placed inside its parent's rect");
    }

    RectTransform *ScrollView::GetContent() const
    {
        if (_content)
        {
            return _content;
        }
        for (const auto &child : GetGameObject().GetChildren())
        {
            if (child)
            {
                if (RectTransform *rectTransform = child->GetComponent<RectTransform>())
                {
                    return rectTransform;
                }
            }
        }
        return nullptr;
    }

    Rect ScrollView::GetViewRect() const
    {
        const RectTransform *view = GetGameObject().GetComponent<RectTransform>();
        return view ? view->GetRect() : Rect{};
    }

    Rect ScrollView::GetContentRect() const
    {
        const RectTransform *content = GetContent();
        return content ? content->GetRect() : Rect{};
    }

    Math::Vector2 ScrollView::GetNormalizedPosition() const
    {
        const Rect view = GetViewRect();
        const Rect content = GetContentRect();
        return Math::Vector2{NormalizedOnAxis(view.x, view.width, content.x, content.width, 0.0f),
                             NormalizedOnAxis(view.y, view.height, content.y, content.height, 1.0f)};
    }

    void ScrollView::SetNormalizedPosition(const Math::Vector2 &position)
    {
        RectTransform *content = GetContent();
        if (!content)
        {
            return;
        }
        const Rect view = GetViewRect();
        const Rect rect = content->GetRect();
        const bool clamp = _movementType != ScrollMovementType::Unrestricted;
        auto delta = [clamp](const float viewMin, const float viewMax, const float contentMin, const float contentMax,
                             const float normalized, const bool pinToMax)
        {
            const float hidden = (contentMax - contentMin) - (viewMax - viewMin);
            if (!(hidden > 0.0f))
            {
                return pinToMax ? viewMax - contentMax : viewMin - contentMin;
            }
            const float n = clamp ? Clamp01(normalized) : normalized;
            return (viewMin - n * hidden) - contentMin;
        };
        const float dx = std::isfinite(position.x) ? delta(view.XMin(), view.XMax(), rect.XMin(), rect.XMax(), position.x, false) : 0.0f;
        const float dy = std::isfinite(position.y) ? delta(view.YMin(), view.YMax(), rect.YMin(), rect.YMax(), position.y, true) : 0.0f;
        const Math::Vector2 current = content->GetAnchoredPosition();
        StopMovement();
        SetContentPosition(Math::Vector2{current.x + dx, current.y + dy}, true);
        UpdateScrollbars();
        if (!_deferNotify)
        {
            NotifyIfMoved();
        }
    }

    void ScrollView::SetHorizontalNormalizedPosition(const float position)
    {
        SetNormalizedPosition(Math::Vector2{position, GetNormalizedPosition().y});
    }

    void ScrollView::SetVerticalNormalizedPosition(const float position)
    {
        SetNormalizedPosition(Math::Vector2{GetNormalizedPosition().x, position});
    }

    Math::Vector2 ScrollView::CalculateOffset(const Math::Vector2 &delta) const
    {
        Math::Vector2 offset{0.0f, 0.0f};
        if (_movementType == ScrollMovementType::Unrestricted)
        {
            return offset;
        }
        const Rect view = GetViewRect();
        const Rect content = GetContentRect();
        if (_horizontal)
        {
            offset.x = OffsetOnAxis(view.XMin(), view.XMax(), content.XMin() + delta.x, content.XMax() + delta.x, false);
        }
        if (_vertical)
        {
            offset.y = OffsetOnAxis(view.YMin(), view.YMax(), content.YMin() + delta.y, content.YMax() + delta.y, true);
        }
        return offset;
    }

    void ScrollView::SetContentPosition(const Math::Vector2 &position, const bool allAxes)
    {
        RectTransform *content = GetContent();
        if (!content)
        {
            return;
        }
        const Math::Vector2 current = content->GetAnchoredPosition();
        const Math::Vector2 next{(_horizontal || allAxes) && std::isfinite(position.x) ? position.x : current.x,
                                 (_vertical || allAxes) && std::isfinite(position.y) ? position.y : current.y};
        if (next.x == current.x && next.y == current.y)
        {
            return;
        }
        content->SetAnchoredPosition(next);
        // The resolved rect moves with it, so the position reads back before the next layout
        Rect rect = content->GetRect();
        rect.x += next.x - current.x;
        rect.y += next.y - current.y;
        content->SetResolvedRect(rect);
        _pendingNotify = true;
    }

    void ScrollView::Scroll(const Math::Vector2 &wheel)
    {
        RectTransform *content = GetContent();
        if (!content || (!_horizontal && !_vertical) || !std::isfinite(wheel.x) || !std::isfinite(wheel.y))
        {
            return;
        }
        // Wheel up (+y) shows content further up: the content moves down. +x moves it right, as Unity's.
        Math::Vector2 delta{wheel.x, -wheel.y};
        if (_vertical && !_horizontal)
        {
            if (std::abs(delta.x) > std::abs(delta.y))
            {
                delta.y = delta.x;
            }
            delta.x = 0.0f;
        }
        if (_horizontal && !_vertical)
        {
            // The vertical wheel scrolls sideways: down shows content further right
            if (std::abs(wheel.y) > std::abs(wheel.x))
            {
                delta.x = wheel.y;
            }
            delta.y = 0.0f;
        }
        const Math::Vector2 current = content->GetAnchoredPosition();
        Math::Vector2 position{current.x + delta.x * _scrollSensitivity, current.y + delta.y * _scrollSensitivity};
        if (_movementType == ScrollMovementType::Clamped)
        {
            const Math::Vector2 offset = CalculateOffset(Math::Vector2{position.x - current.x, position.y - current.y});
            position = Math::Vector2{position.x + offset.x, position.y + offset.y};
        }
        SetContentPosition(position);
        UpdateScrollbars();
        if (!_deferNotify)
        {
            NotifyIfMoved();
        }
    }

    void ScrollView::BeginDrag(const Math::Vector2 &pointer)
    {
        const RectTransform *content = GetContent();
        if (!content)
        {
            return;
        }
        _dragging = true;
        _dragStartPointer = pointer;
        _dragStartContent = content->GetAnchoredPosition();
        StopMovement();
        _hasPrevious = false;
    }

    void ScrollView::Drag(const Math::Vector2 &pointer)
    {
        const RectTransform *content = GetContent();
        if (!_dragging || !content)
        {
            return;
        }
        const Math::Vector2 current = content->GetAnchoredPosition();
        Math::Vector2 position{_dragStartContent.x + (pointer.x - _dragStartPointer.x),
                               _dragStartContent.y + (pointer.y - _dragStartPointer.y)};
        const Math::Vector2 offset = CalculateOffset(Math::Vector2{position.x - current.x, position.y - current.y});
        position = Math::Vector2{position.x + offset.x, position.y + offset.y};
        if (_movementType == ScrollMovementType::Elastic)
        {
            // Past an edge the content follows less and less (Unity's rubber band)
            const Rect view = GetViewRect();
            if (offset.x != 0.0f)
            {
                position.x -= RubberDelta(offset.x, view.width);
            }
            if (offset.y != 0.0f)
            {
                position.y -= RubberDelta(offset.y, view.height);
            }
        }
        SetContentPosition(position);
        UpdateScrollbars();
        if (!_deferNotify)
        {
            NotifyIfMoved();
        }
    }

    void ScrollView::EndDrag()
    {
        _dragging = false;
    }

    void ScrollView::Step(const float deltaTime)
    {
        RectTransform *content = GetContent();
        if (!content)
        {
            return;
        }
        if (deltaTime > 0.0f && std::isfinite(deltaTime))
        {
            const Math::Vector2 current = content->GetAnchoredPosition();
            const Math::Vector2 offset = CalculateOffset(Math::Vector2{0.0f, 0.0f});
            const bool hasOffset = std::abs(offset.x) > 0.0f || std::abs(offset.y) > 0.0f;
            const bool moving = _velocity.x != 0.0f || _velocity.y != 0.0f;
            if (!_dragging && (hasOffset || moving))
            {
                float position[2] = {current.x, current.y};
                float velocity[2] = {_velocity.x, _velocity.y};
                const float offsets[2] = {offset.x, offset.y};
                const bool enabled[2] = {_horizontal, _vertical};
                for (int axis = 0; axis < 2; ++axis)
                {
                    if (!enabled[axis])
                    {
                        velocity[axis] = 0.0f;
                        continue;
                    }
                    if (_movementType == ScrollMovementType::Elastic && offsets[axis] != 0.0f)
                    {
                        if (std::abs(offsets[axis]) < SettleDistance)
                        {
                            position[axis] += offsets[axis]; // there: no endless approach
                            velocity[axis] = 0.0f;
                            continue;
                        }
                        float speed = velocity[axis];
                        position[axis] = SmoothDamp(position[axis], position[axis] + offsets[axis], speed,
                                                    std::max(_elasticity, 0.0f), deltaTime);
                        velocity[axis] = std::abs(speed) < StopSpeed ? 0.0f : speed;
                    }
                    else if (_inertia)
                    {
                        velocity[axis] *= std::pow(std::clamp(_decelerationRate, 0.0f, 1.0f), deltaTime);
                        if (std::abs(velocity[axis]) < StopSpeed)
                        {
                            velocity[axis] = 0.0f;
                        }
                        position[axis] += velocity[axis] * deltaTime;
                    }
                    else
                    {
                        velocity[axis] = 0.0f;
                    }
                }
                Math::Vector2 next{position[0], position[1]};
                if (_movementType == ScrollMovementType::Clamped)
                {
                    const Math::Vector2 clampOffset = CalculateOffset(Math::Vector2{next.x - current.x, next.y - current.y});
                    next = Math::Vector2{next.x + clampOffset.x, next.y + clampOffset.y};
                    // Stopped at an edge: no speed into it
                    if (clampOffset.x != 0.0f)
                    {
                        velocity[0] = 0.0f;
                    }
                    if (clampOffset.y != 0.0f)
                    {
                        velocity[1] = 0.0f;
                    }
                }
                _velocity = Math::Vector2{velocity[0], velocity[1]};
                SetContentPosition(next);
            }

            // While dragging, the speed the content moves at, smoothed, for the coast after the release
            const Math::Vector2 now = content->GetAnchoredPosition();
            if (_dragging && _inertia && _hasPrevious)
            {
                const float blend = std::min(deltaTime * 10.0f, 1.0f);
                const Math::Vector2 measured{(now.x - _previousPosition.x) / deltaTime, (now.y - _previousPosition.y) / deltaTime};
                _velocity = Math::Vector2{_velocity.x + (measured.x - _velocity.x) * blend,
                                          _velocity.y + (measured.y - _velocity.y) * blend};
            }
            _previousPosition = now;
            _hasPrevious = true;
        }
        UpdateScrollbars();
        if (!_deferNotify)
        {
            NotifyIfMoved();
        }
    }

    void ScrollView::UpdateScrollbars()
    {
        if (!_verticalScrollbar && !_horizontalScrollbar)
        {
            return;
        }
        const Rect view = GetViewRect();
        const Rect content = GetContentRect();
        const Math::Vector2 normalized = GetNormalizedPosition();
        // The handle's share of the track is the share of the content in view; it travels the rest
        auto place = [](RectTransform &handle, const float viewSize, const float contentSize, const float position,
                        const bool vertical)
        {
            const float size = contentSize > 0.0f ? Clamp01(viewSize / contentSize) : 1.0f;
            const float start = Clamp01(position) * (1.0f - size);
            if (vertical)
            {
                handle.SetAnchorMin(Math::Vector2{0.0f, start});
                handle.SetAnchorMax(Math::Vector2{1.0f, start + size});
            }
            else
            {
                handle.SetAnchorMin(Math::Vector2{start, 0.0f});
                handle.SetAnchorMax(Math::Vector2{start + size, 1.0f});
            }
            handle.SetAnchoredPosition(Math::Vector2{0.0f, 0.0f});
            handle.SetSizeDelta(Math::Vector2{0.0f, 0.0f});
        };
        if (_verticalScrollbar)
        {
            place(*_verticalScrollbar, view.height, content.height, normalized.y, true);
        }
        if (_horizontalScrollbar)
        {
            place(*_horizontalScrollbar, view.width, content.width, normalized.x, false);
        }
    }

    void ScrollView::NotifyIfMoved()
    {
        if (!_pendingNotify)
        {
            return;
        }
        _pendingNotify = false;
        const Math::Vector2 position = GetNormalizedPosition();
        // Held for the invoke: a listener may destroy this component. Nothing after it touches the component.
        const std::shared_ptr<Base::EventHandler<Math::Vector2>> onValueChanged = _onValueChanged;
        (*onValueChanged)(position);
    }

    ScrollView::ListenerId ScrollView::AddOnValueChanged(std::function<void(Math::Vector2)> listener)
    {
        return *_onValueChanged += [listener = std::move(listener)](const Math::Vector2 position)
        {
            try
            {
                listener(position);
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::format("ScrollView OnValueChanged listener threw: {}", e.what()));
            }
            catch (...)
            {
                Logger::Error("ScrollView OnValueChanged listener threw an unknown exception");
            }
        };
    }

    void ScrollView::RemoveOnValueChanged(const ListenerId id)
    {
        *_onValueChanged -= id;
    }

    void ScrollView::ClearOnValueChanged()
    {
        _onValueChanged->Clear();
    }

    std::size_t ScrollView::GetOnValueChangedListenerCount() const
    {
        return _onValueChanged->GetSubscriberCount();
    }

    float ScrollView::RubberDelta(const float overStretching, const float viewSize)
    {
        if (!(viewSize > 0.0f))
        {
            return 0.0f;
        }
        const float magnitude = (1.0f - 1.0f / (std::abs(overStretching) * 0.55f / viewSize + 1.0f)) * viewSize;
        return overStretching < 0.0f ? -magnitude : magnitude;
    }

    float ScrollView::SmoothDamp(const float current, const float target, float &velocity, const float smoothTime,
                                 const float deltaTime)
    {
        const float time = std::max(0.0001f, smoothTime);
        const float omega = 2.0f / time;
        const float x = omega * deltaTime;
        const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float change = current - target;
        const float temp = (velocity + omega * change) * deltaTime;
        velocity = (velocity - omega * temp) * decay;
        float output = target + (change + temp) * decay;
        // Never past the target
        if ((target - current > 0.0f) == (output > target))
        {
            output = target;
            velocity = deltaTime > 0.0f ? (output - target) / deltaTime : 0.0f;
        }
        return output;
    }

    void ScrollView::OnDisable()
    {
        _dragging = false;
        _pressTracked = false;
        _draggedHandle = nullptr;
        StopMovement();
    }

    void ScrollView::OnUpdate()
    {
        if (!IsActive())
        {
            return;
        }
        const float deltaTime = Time::GetUnscaledDeltaTime();
        // The listeners are called once, at the end, so none of them can destroy this component mid-frame
        _deferNotify = true;
        ProcessPointer();
        Step(deltaTime);
        _deferNotify = false;
        NotifyIfMoved();
    }

    void ScrollView::ProcessPointer()
    {
        const Input::Mouse *mouse = Input::Mouse::Get();
        if (!mouse)
        {
            return;
        }
        Application &application = Application::GetInstance();
        const Input::PointerDispatcher &dispatcher = application.GetPointerDispatcher();
        GameObject &self = GetGameObject();

        // The wheel, over the view or anything below it
        const Math::Vector2 wheel = mouse->GetScrollDelta();
        if ((wheel.x != 0.0f || wheel.y != 0.0f) && IsSelfOrBelow(dispatcher.GetHovered(), self))
        {
            Scroll(wheel);
        }

        if (!mouse->GetButton(0))
        {
            // Released: the drag ends and the content coasts (Step)
            _dragging = false;
            _pressTracked = false;
            _draggedHandle = nullptr;
            return;
        }

        // The pointer in canvas space: overlay canvases only
        const Canvas *canvas = RootCanvasOf(self);
        if (!canvas || canvas->IsWorldSpace())
        {
            return;
        }
        const Math::Vector2 pointer =
            UISystem::WindowToCanvas(mouse->GetPosition(), application.GetWindow().GetRenderDimensions());

        GameObject *captured = dispatcher.GetCaptured();
        if (!_pressTracked && !_dragging && !_draggedHandle && captured)
        {
            if (_verticalScrollbar && captured == &_verticalScrollbar->GetGameObject())
            {
                _draggedHandle = _verticalScrollbar;
            }
            else if (_horizontalScrollbar && captured == &_horizontalScrollbar->GetGameObject())
            {
                _draggedHandle = _horizontalScrollbar;
            }
            else if (IsSelfOrBelow(captured, self))
            {
                _pressTracked = true;
                _pressPointer = pointer;
            }
            if (_draggedHandle)
            {
                _handleStartPointer = pointer;
                _handleStartPosition = GetNormalizedPosition();
            }
        }

        if (_draggedHandle)
        {
            // Across the track: the handle's travel is the track less the handle
            const GameObject::Ptr trackObject = _draggedHandle->GetGameObject().GetParent();
            const RectTransform *track = trackObject ? trackObject->GetComponent<RectTransform>() : nullptr;
            if (!track)
            {
                return;
            }
            const Rect view = GetViewRect();
            const Rect content = GetContentRect();
            const bool vertical = _draggedHandle == _verticalScrollbar;
            const float viewSize = vertical ? view.height : view.width;
            const float contentSize = vertical ? content.height : content.width;
            const float size = contentSize > 0.0f ? Clamp01(viewSize / contentSize) : 1.0f;
            const float travel = (vertical ? track->GetRect().height : track->GetRect().width) * (1.0f - size);
            if (travel > 0.0f)
            {
                if (vertical)
                {
                    SetVerticalNormalizedPosition(_handleStartPosition.y + (pointer.y - _handleStartPointer.y) / travel);
                }
                else
                {
                    SetHorizontalNormalizedPosition(_handleStartPosition.x + (pointer.x - _handleStartPointer.x) / travel);
                }
            }
            return;
        }

        if (_pressTracked && !_dragging)
        {
            const float dx = pointer.x - _pressPointer.x;
            const float dy = pointer.y - _pressPointer.y;
            if (dx * dx + dy * dy >= _dragThreshold * _dragThreshold)
            {
                // A drag, not a click: the buttons under the press (up to this view) let it go
                for (GameObject *object = captured; object;)
                {
                    for (Button *button : object->GetComponents<Button>())
                    {
                        if (button)
                        {
                            button->CancelPress();
                        }
                    }
                    if (object == &self)
                    {
                        break;
                    }
                    const GameObject::Ptr parent = object->GetParent();
                    object = parent.get();
                }
                BeginDrag(pointer);
            }
        }
        if (_dragging)
        {
            Drag(pointer);
        }
    }
}
