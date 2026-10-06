#include "engine/input/PointerDispatcher.hpp"

#include <cmath>
#include <utility>
#include <vector>

#include <math/Ray.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2Engine::Input
{
    namespace
    {
        /// The object, held for the frame, if it can receive pointer events: owned by a shared_ptr, not
        /// destroyed and active in the hierarchy. Otherwise null.
        GameObject::Ptr Receivable(const GameObject *gameObject)
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

        GameObject::Ptr Receivable(const std::weak_ptr<GameObject> &gameObject)
        {
            return Receivable(gameObject.lock().get());
        }

        struct ProcessingScope
        {
            bool &flag;
            explicit ProcessingScope(bool &f) : flag(f) { flag = true; }
            ~ProcessingScope() { flag = false; }
            ProcessingScope(const ProcessingScope &) = delete;
            ProcessingScope &operator=(const ProcessingScope &) = delete;
        };
    }

    PointerState PointerState::FromMouse(const Mouse &mouse)
    {
        PointerState state;
        state.position = mouse.GetPosition();
        state.held = mouse.GetButton(0);
        state.pressed = mouse.GetButtonDown(0);
        state.released = mouse.GetButtonUp(0);
        return state;
    }

    void PointerDispatcher::Update()
    {
        const Mouse *mouse = Mouse::Get();
        if (!mouse)
        {
            return;
        }
        Process(PointerState::FromMouse(*mouse));
    }

    void PointerDispatcher::Process(const PointerState &state)
    {
        if (_processing)
        {
            return;
        }
        ProcessingScope processing{_processing};

        // Reset() from a provider or callback ends this frame's processing: nothing after it may send to, or
        // re-record, the objects it forgot
        const uint64_t resetsAtStart = _resetCount;
        const auto wasReset = [this, resetsAtStart] { return _resetCount != resetsAtStart; };

        // Destroyed or deactivated since the last frame: dropped without OnMouseExit/OnMouseUp, as physics
        // drops an inactive object's pairs without telling it
        GameObject::Ptr hovered = Receivable(_hovered);
        if (!hovered)
        {
            _hovered.reset();
        }
        GameObject::Ptr captured = Receivable(_captured);
        if (!captured)
        {
            _captured.reset();
        }

        // This frame's target: UI first, then the world. The providers are called through copies, so one that
        // replaces itself (SetWorldHitProvider(nullptr)) doesn't destroy the function that is running.
        GameObject::Ptr target;
        _pointerOverUI = false;
        if (const HitProvider uiProvider = _uiProvider)
        {
            if (const GameObject *uiHit = uiProvider(state.position))
            {
                _pointerOverUI = true;
                target = Receivable(uiHit);
            }
        }
        if (!_pointerOverUI)
        {
            const HitProvider worldProvider = _worldProvider;
            target = Receivable(worldProvider ? worldProvider(state.position) : PickWithMainCamera(state.position));
        }
        if (wasReset())
        {
            return;
        }

        // Sends one callback; false once a Reset() means the frame must stop
        const auto send = [&wasReset](GameObject &gameObject, void (Component::*callback)())
        {
            Send(gameObject, callback);
            return !wasReset();
        };

        // The button, in Unity's order: Down (capturing the target), else Up/UpAsButton on release, else Drag
        if (state.pressed)
        {
            if (captured)
            {
                // Still captured, so a release went unseen (frames without a Process): end it first
                _captured.reset();
                if (!send(*captured, &Component::OnMouseUp))
                {
                    return;
                }
            }
            if (target)
            {
                _captured = target;
                if (!send(*target, &Component::OnMouseDown))
                {
                    return;
                }
            }
        }
        else if (!state.held)
        {
            if (captured)
            {
                _captured.reset();
                if (target == captured && !send(*captured, &Component::OnMouseUpAsButton))
                {
                    return;
                }
                if (!send(*captured, &Component::OnMouseUp))
                {
                    return;
                }
            }
        }
        else if (captured)
        {
            if (!send(*captured, &Component::OnMouseDrag))
            {
                return;
            }
        }

        // Then hover. A button callback may have destroyed or deactivated either object since they were looked
        // up; such an object is dropped like one that went before the frame.
        hovered = Receivable(hovered.get());
        target = Receivable(target.get());
        _hovered = target;
        if (target == hovered)
        {
            if (target)
            {
                send(*target, &Component::OnMouseOver);
            }
            return;
        }
        if (hovered && !send(*hovered, &Component::OnMouseExit))
        {
            return;
        }
        if (target && send(*target, &Component::OnMouseEnter))
        {
            send(*target, &Component::OnMouseOver);
        }
    }

    GameObject* PointerDispatcher::GetHovered() const
    {
        return Receivable(_hovered).get();
    }

    GameObject* PointerDispatcher::GetCaptured() const
    {
        return Receivable(_captured).get();
    }

    void PointerDispatcher::Reset()
    {
        ++_resetCount; // a Process under way stops sending
        _hovered.reset();
        _captured.reset();
        _pointerOverUI = false;
    }

    GameObject* PointerDispatcher::PickWorld(const Camera &camera, const Math::Vector2 &screenPosition,
                                             const Vector2i &viewport, const uint32_t mask)
    {
        if (viewport[0] <= 0 || viewport[1] <= 0)
        {
            return nullptr; // minimised, or no window
        }
        // Only inside the viewport, as Unity only picks inside the camera's pixel rect. Outside the window GLFW
        // still reports the cursor (beyond the edges, even negative), which would cast rays outside the view.
        if (!(screenPosition.x >= 0.0f && screenPosition.x < static_cast<float>(viewport[0]) &&
              screenPosition.y >= 0.0f && screenPosition.y < static_cast<float>(viewport[1])))
        {
            return nullptr;
        }

        // Near plane to far plane, as in Unity: what the camera can't see can't be clicked
        float nearToFar = 0.0f;
        const Math::Ray ray = camera.ScreenPointToRay(screenPosition, viewport, &nearToFar);
        if (!(nearToFar > 0.0f) || !std::isfinite(nearToFar))
        {
            return nullptr;
        }

        Physics::RaycastHit hit;
        if (!Physics::Raycast::Single(ray.origin, ray.direction, hit, nearToFar, mask))
        {
            return nullptr;
        }
        // The collider's own object, which for a child collider is not the body's owner (hit.gameObject)
        if (hit.collider)
        {
            return &hit.collider->GetGameObject();
        }
        return hit.gameObject;
    }

    GameObject* PointerDispatcher::PickWithMainCamera(const Math::Vector2 &screenPosition) const
    {
        auto &application = Application::GetInstance();
        const Camera *camera = application.GetMainCamera();
        if (!camera)
        {
            return nullptr;
        }
        // Window coordinates, the cursor's space (not the framebuffer, which differs on high-DPI displays), or the
        // host's render size when Window::SetRenderSize set one (the editor's viewport), as the frame is drawn
        return PickWorld(*camera, screenPosition, application.GetWindow().GetRenderDimensions(), _pickMask);
    }

    void PointerDispatcher::Send(GameObject &gameObject, void (Component::*callback)())
    {
        // Held for the call, and the component list snapshotted: a callback can add or remove components or
        // destroy the object. Lifetime tokens tell which of the snapshot are still alive.
        const GameObject::Ptr keepAlive = Receivable(&gameObject);
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
            // Like physics events: an object made inactive or destroyed by an earlier callback gets no more,
            // but a disabled component on an active object still does
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
}
