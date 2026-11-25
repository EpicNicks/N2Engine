#include "engine/ui/UINavigator.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <math/Vector2.hpp>

#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/input/GamepadSource.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/Keyboard.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/Slider.hpp"
#include "engine/ui/Transform2D.hpp"
#include "engine/ui/UISystem.hpp"

namespace N2Engine::UI
{
    namespace
    {
        /// `gameObject` held, when it can be selected at all: alive, not destroyed and active in the hierarchy
        GameObject::Ptr Alive(const GameObject *gameObject)
        {
            if (!gameObject)
            {
                return nullptr;
            }
            GameObject::Ptr held = std::const_pointer_cast<GameObject>(gameObject->weak_from_this().lock());
            if (!held || held->IsDestroyed() || !held->IsActiveInHierarchy())
            {
                return nullptr;
            }
            return held;
        }

        /// Calls `callback` on every component of an active object, as the pointer messages are delivered: an earlier
        /// callback may destroy components or deactivate the object
        void Send(GameObject &gameObject, void (Component::*callback)())
        {
            const GameObject::Ptr keepAlive = Alive(&gameObject);
            if (!keepAlive)
            {
                return;
            }
            std::vector<std::pair<Component *, std::weak_ptr<const bool>>> components;
            components.reserve(gameObject.GetAllComponents().size());
            for (const auto &component : gameObject.GetAllComponents())
            {
                components.emplace_back(component.get(), component->GetLifetimeToken());
            }
            for (const auto &[component, lifetime] : components)
            {
                if (gameObject.IsDestroyed() || !gameObject.IsActiveInHierarchy())
                {
                    return;
                }
                if (lifetime.expired() || component->IsDestroyed())
                {
                    continue;
                }
                (component->*callback)();
            }
        }

        NavigationMode ModeOf(const GameObject &gameObject)
        {
            const Navigation *navigation = gameObject.GetComponent<Navigation>();
            return navigation && navigation->IsActive() ? navigation->GetMode() : NavigationMode::Automatic;
        }

        /// The box an element is drawn in (canvas space), nothing without a rect that shows
        std::optional<Rect> DrawnBounds(const GameObject &gameObject)
        {
            const RectTransform *rectTransform = gameObject.GetComponent<RectTransform>();
            if (!rectTransform || !rectTransform->GetRect().HasArea() || !rectTransform->GetLocalToCanvas().Inverse())
            {
                return std::nullopt;
            }
            return BoundsOf(TransformedCorners(rectTransform->GetRect(), rectTransform->GetLocalToCanvas()));
        }

        void Gather(const GameObject::Ptr &gameObject, std::vector<GameObject *> &out)
        {
            if (!gameObject || !gameObject->IsActiveInHierarchy())
            {
                return;
            }
            if (UINavigator::IsSelectable(*gameObject) && ModeOf(*gameObject) != NavigationMode::None &&
                DrawnBounds(*gameObject))
            {
                out.push_back(gameObject.get());
            }
            for (const auto &child : gameObject->GetChildren())
            {
                Gather(child, out);
            }
        }

        /// The value a held slider moves by with one press
        float SliderStep(const Slider &slider)
        {
            const float range = std::abs(slider.GetMaxValue() - slider.GetMinValue());
            return slider.GetWholeNumbers() ? 1.0f : range / 10.0f;
        }

        /// Scrolls every ScrollView above `target` far enough to show it
        void ScrollIntoView(const Scene &scene, const Vector2i &viewport, GameObject &target)
        {
            for (GameObject::Ptr parent = target.GetParent(); parent; parent = parent->GetParent())
            {
                ScrollView *scrollView = parent->GetComponent<ScrollView>();
                const RectTransform *view = parent->GetComponent<RectTransform>();
                if (!scrollView || !scrollView->IsActive() || !view)
                {
                    continue;
                }
                RectTransform *content = scrollView->GetContent();
                const RectTransform *shown = target.GetComponent<RectTransform>();
                if (!content || !shown || !UISystem::IsSelfOrBelow(&target, content->GetGameObject()))
                {
                    continue;
                }
                // The target in the view's own space
                ConvexPolygon corners = TransformedCorners(shown->GetRect(), shown->GetLocalToCanvas());
                bool placed = true;
                for (Math::Vector2 &corner : corners)
                {
                    const std::optional<Math::Vector2> local = view->CanvasToLocal(corner);
                    placed = placed && local.has_value();
                    corner = local.value_or(Math::Vector2{});
                }
                if (!placed)
                {
                    continue;
                }
                const Rect item = BoundsOf(corners);
                const Rect &window = view->GetRect();
                // Along each axis: show the start of an item larger than the view (its left, or its top)
                Math::Vector2 move{0.0f, 0.0f};
                if (scrollView->GetHorizontal())
                {
                    if (item.XMin() < window.XMin() || (item.width > window.width && item.XMin() > window.XMin()))
                    {
                        move.x = window.XMin() - item.XMin();
                    }
                    else if (item.XMax() > window.XMax() && item.width <= window.width)
                    {
                        move.x = window.XMax() - item.XMax();
                    }
                }
                if (scrollView->GetVertical())
                {
                    if (item.YMax() > window.YMax() || (item.height > window.height && item.YMax() < window.YMax()))
                    {
                        move.y = window.YMax() - item.YMax();
                    }
                    else if (item.YMin() < window.YMin() && item.height <= window.height)
                    {
                        move.y = window.YMin() - item.YMin();
                    }
                }
                if (move.x == 0.0f && move.y == 0.0f)
                {
                    continue;
                }
                const Math::Vector2 position = content->GetAnchoredPosition();
                content->SetAnchoredPosition(Math::Vector2{position.x + move.x, position.y + move.y});
                scrollView->StopMovement();
                // The rects the next view up looks at
                static_cast<void>(UISystem::CollectGraphics(scene, viewport));
            }
        }
    }

    bool UINavigator::IsSelectable(const GameObject &gameObject)
    {
        if (!gameObject.IsActiveInHierarchy() || !gameObject.GetComponent<RectTransform>())
        {
            return false;
        }
        for (const Button *button : gameObject.GetComponents<Button>())
        {
            if (button && button->IsActive() && button->IsInteractable())
            {
                return true;
            }
        }
        for (const Slider *slider : gameObject.GetComponents<Slider>())
        {
            if (slider && slider->IsActive() && slider->IsInteractable())
            {
                return true;
            }
        }
        return false;
    }

    std::vector<GameObject *> UINavigator::CollectSelectables(const Scene &scene, const Vector2i &viewport,
                                                              const GameObject *within)
    {
        std::vector<GameObject *> result;
        if (!UISystem::IsInBuild())
        {
            return result;
        }
        const Canvas *current = within ? UISystem::RootCanvasOf(*within) : nullptr;
        const bool world = current && current->IsWorldSpace();
        if (world)
        {
            static_cast<void>(UISystem::CollectWorldCanvasGraphics(*current)); // lays it out
        }
        else
        {
            static_cast<void>(UISystem::CollectGraphics(scene, viewport)); // lays out the overlay canvases
        }
        for (Canvas *canvas : UISystem::RootCanvases(scene))
        {
            if (world ? canvas == current : !canvas->IsWorldSpace())
            {
                Gather(canvas->GetGameObject().shared_from_this(), result);
            }
        }
        return result;
    }

    GameObject *UINavigator::GetSelected() const
    {
        return Alive(_selected.lock().get()).get();
    }

    void UINavigator::SetSelected(GameObject *gameObject)
    {
        const GameObject::Ptr next = Alive(gameObject);
        const GameObject::Ptr previous = Alive(_selected.lock().get());
        if (next == previous)
        {
            if (!next)
            {
                _selected.reset();
            }
            return;
        }
        _selected = next;
        if (previous)
        {
            Send(*previous, &Component::OnDeselect);
        }
        // A callback may have selected something else meanwhile: that one has had its OnSelect
        if (next && _selected.lock() == next)
        {
            Send(*next, &Component::OnSelect);
        }
    }

    void UINavigator::SelectAndShow(const Scene &scene, const Vector2i &viewport, GameObject &gameObject)
    {
        const GameObject::Ptr held = Alive(&gameObject);
        if (!held)
        {
            return;
        }
        ScrollIntoView(scene, viewport, *held);
        SetSelected(held.get());
    }

    bool UINavigator::Move(const Scene &scene, const Vector2i &viewport, const NavigationDirection direction)
    {
        GameObject *selected = GetSelected();
        if (!selected)
        {
            return false;
        }
        // A slider takes the arrows along its bar
        for (Slider *slider : selected->GetComponents<Slider>())
        {
            if (!slider || !slider->IsActive() || !slider->IsInteractable())
            {
                continue;
            }
            const SliderDirection bar = slider->GetDirection();
            const bool vertical = bar == SliderDirection::BottomToTop || bar == SliderDirection::TopToBottom;
            const bool along = vertical ? direction == NavigationDirection::Up || direction == NavigationDirection::Down
                                        : direction == NavigationDirection::Left || direction == NavigationDirection::Right;
            if (along)
            {
                const bool forward = direction == NavigationDirection::Up || direction == NavigationDirection::Right;
                const bool reversed = bar == SliderDirection::RightToLeft || bar == SliderDirection::TopToBottom;
                const float step = SliderStep(*slider);
                slider->SetValue(slider->GetValue() + (forward != reversed ? step : -step));
                return true;
            }
            break;
        }

        const NavigationMode mode = ModeOf(*selected);
        if (mode == NavigationMode::None)
        {
            return false;
        }
        if (mode == NavigationMode::Explicit)
        {
            const RectTransform *neighbor = selected->GetComponent<Navigation>()->GetNeighbor(direction);
            const GameObject::Ptr target = neighbor ? Alive(&neighbor->GetGameObject()) : nullptr;
            if (!target || target.get() == selected)
            {
                return false;
            }
            static_cast<void>(CollectSelectables(scene, viewport, target.get())); // fresh rects to scroll by
            SelectAndShow(scene, viewport, *target);
            return true;
        }

        const std::vector<GameObject *> candidates = CollectSelectables(scene, viewport, selected);
        const std::optional<Rect> from = DrawnBounds(*selected);
        if (!from)
        {
            return false;
        }
        Math::Vector2 along{0.0f, 0.0f};
        switch (direction)
        {
        case NavigationDirection::Up:
            along.y = 1.0f;
            break;
        case NavigationDirection::Down:
            along.y = -1.0f;
            break;
        case NavigationDirection::Left:
            along.x = -1.0f;
            break;
        case NavigationDirection::Right:
            along.x = 1.0f;
            break;
        }
        // From the middle of the selected element's edge on that side
        const Math::Vector2 edge{from->x + from->width * 0.5f * (1.0f + along.x),
                                 from->y + from->height * 0.5f * (1.0f + along.y)};
        GameObject *best = nullptr;
        float bestScore = 0.0f;
        for (GameObject *candidate : candidates)
        {
            if (candidate == selected)
            {
                continue;
            }
            const std::optional<Rect> bounds = DrawnBounds(*candidate);
            if (!bounds)
            {
                continue;
            }
            const Math::Vector2 offset{bounds->x + bounds->width * 0.5f - edge.x,
                                       bounds->y + bounds->height * 0.5f - edge.y};
            const float dot = along.x * offset.x + along.y * offset.y;
            const float distance = offset.x * offset.x + offset.y * offset.y;
            if (dot <= 0.0f || !(distance > 0.0f))
            {
                continue;
            }
            const float score = dot / distance;
            if (score > bestScore)
            {
                bestScore = score;
                best = candidate;
            }
        }
        if (!best)
        {
            return false;
        }
        SelectAndShow(scene, viewport, *best);
        return true;
    }

    bool UINavigator::Step(const Scene &scene, const Vector2i &viewport, const bool backwards)
    {
        const GameObject *selected = GetSelected();
        if (!selected)
        {
            return false;
        }
        const std::vector<GameObject *> candidates = CollectSelectables(scene, viewport, selected);
        if (candidates.empty())
        {
            return false;
        }
        const auto found = std::ranges::find(candidates, selected);
        std::size_t next = backwards ? candidates.size() - 1 : 0;
        if (found != candidates.end())
        {
            const auto index = static_cast<std::size_t>(found - candidates.begin());
            next = (index + (backwards ? candidates.size() - 1 : 1)) % candidates.size();
        }
        if (candidates[next] == selected)
        {
            return false;
        }
        SelectAndShow(scene, viewport, *candidates[next]);
        return true;
    }

    void UINavigator::Submit()
    {
        if (GameObject *selected = GetSelected())
        {
            Send(*selected, &Component::OnSubmit);
        }
    }

    void UINavigator::Cancel()
    {
        if (GameObject *selected = GetSelected())
        {
            Send(*selected, &Component::OnCancel);
        }
    }

    bool UINavigator::Fires(const std::size_t index, const bool held, const float deltaTime)
    {
        float &wait = _repeat[index];
        if (!held)
        {
            wait = -1.0f;
            return false;
        }
        if (wait < 0.0f)
        {
            wait = std::max(0.0f, _repeatDelay);
            return true;
        }
        wait -= std::isfinite(deltaTime) && deltaTime > 0.0f ? deltaTime : 0.0f;
        if (wait > 0.0f)
        {
            return false;
        }
        // Once a frame at most, however long the frame was
        wait = std::max(0.0f, _repeatInterval);
        return true;
    }

    void UINavigator::Process(const Scene &scene, const Vector2i &viewport, const NavigationInput &input,
                              const float unscaledDeltaTime)
    {
        _submitPressed = false;
        _cancelPressed = false;
        // The timers run whether or not anything is selected, so a key already held when a script selects something
        // waits for its repeat instead of moving at once
        const bool held[6] = {input.up, input.down, input.left, input.right, input.next, input.previous};
        bool fired[6] = {};
        for (std::size_t i = 0; i < 6; ++i)
        {
            fired[i] = Fires(i, held[i], unscaledDeltaTime);
        }
        if (!_enabled || !GetSelected())
        {
            return;
        }

        // A click moves the selection to what was clicked
        for (const GameObject *object = input.pointerPressed; object;)
        {
            if (IsSelectable(*object))
            {
                SetSelected(const_cast<GameObject *>(object));
                break;
            }
            const GameObject::Ptr parent = object->GetParent();
            object = parent.get();
        }

        constexpr NavigationDirection directions[4] = {NavigationDirection::Up, NavigationDirection::Down,
                                                       NavigationDirection::Left, NavigationDirection::Right};
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (fired[i])
            {
                static_cast<void>(Move(scene, viewport, directions[i]));
            }
        }
        if (fired[4])
        {
            static_cast<void>(Step(scene, viewport, false));
        }
        if (fired[5])
        {
            static_cast<void>(Step(scene, viewport, true));
        }
        if (input.submit && GetSelected())
        {
            _submitPressed = true;
            Submit();
        }
        if (input.cancel && GetSelected())
        {
            _cancelPressed = true;
            Cancel();
        }
    }

    NavigationInput UINavigator::ReadInput()
    {
        NavigationInput input;
        if (const Input::Keyboard *keyboard = Input::Keyboard::Get())
        {
            using Input::Key;
            input.up = keyboard->GetKey(Key::Up);
            input.down = keyboard->GetKey(Key::Down);
            input.left = keyboard->GetKey(Key::Left);
            input.right = keyboard->GetKey(Key::Right);
            const bool shift = keyboard->GetKey(Key::LeftShift) || keyboard->GetKey(Key::RightShift);
            const bool tab = keyboard->GetKey(Key::Tab);
            input.next = tab && !shift;
            input.previous = tab && shift;
            input.submit = keyboard->GetKeyDown(Key::Enter) || keyboard->GetKeyDown(Key::KpEnter) ||
                           keyboard->GetKeyDown(Key::Space);
            input.cancel = keyboard->GetKeyDown(Key::Escape);
        }

        const Input::InputDevice *device = Application::GetInstance().GetWindow().GetInputDevice();
        bool submit = false;
        bool cancel = false;
        for (int id = 0; id < Input::MaxGamepads; ++id)
        {
            Input::GamepadState state;
            if (!Input::ReadGamepadState(device, id, state))
            {
                continue;
            }
            using Input::GamepadAxis;
            using Input::GamepadButton;
            // Sticks are y down: up is negative
            input.up = input.up || state.Button(GamepadButton::DpadUp) || state.Axis(GamepadAxis::LeftY) < -StickThreshold;
            input.down = input.down || state.Button(GamepadButton::DpadDown) ||
                         state.Axis(GamepadAxis::LeftY) > StickThreshold;
            input.left = input.left || state.Button(GamepadButton::DpadLeft) ||
                         state.Axis(GamepadAxis::LeftX) < -StickThreshold;
            input.right = input.right || state.Button(GamepadButton::DpadRight) ||
                          state.Axis(GamepadAxis::LeftX) > StickThreshold;
            submit = submit || state.Button(GamepadButton::South);
            cancel = cancel || state.Button(GamepadButton::East);
        }
        input.submit = input.submit || (submit && !_padSubmit);
        input.cancel = input.cancel || (cancel && !_padCancel);
        _padSubmit = submit;
        _padCancel = cancel;

        if (const Input::Mouse *mouse = Input::Mouse::Get(); mouse && mouse->GetButtonDown(0))
        {
            input.pointerPressed = Application::GetInstance().GetPointerDispatcher().GetCaptured();
        }
        return input;
    }

    void UINavigator::Update()
    {
        if (!UISystem::IsInBuild() || SceneManager::GetCurSceneIndex() == -1)
        {
            return;
        }
        const NavigationInput input = ReadInput();
        Process(SceneManager::GetCurSceneRef(), Application::GetInstance().GetWindow().GetRenderDimensions(), input,
                Time::GetUnscaledDeltaTime());
    }
}
