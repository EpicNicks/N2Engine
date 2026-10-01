#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <math/Vector2.hpp>

#include "engine/Camera.hpp"
#include "engine/Layers.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;
}

namespace N2Engine::Input
{
    class Mouse;

    /// One frame of the pointer as the dispatcher reads it: the cursor and the left button's state and edges
    struct PointerState
    {
        Math::Vector2 position{0.0f, 0.0f}; // window coordinates (Mouse::GetPosition)
        bool held = false;                  // the left button is down
        bool pressed = false;               // it went down this frame
        bool released = false;              // it went up this frame

        /// The left button (0) of a mouse, as of its last Update
        [[nodiscard]] static PointerState FromMouse(const Mouse &mouse);
    };

    /**
     * Sends Unity's OnMouse* messages (Component::OnMouseEnter/Over/Exit/Down/Drag/Up/UpAsButton) to the
     * object under the pointer. Application owns one and runs it once per frame, after input polling and the
     * fixed steps and before Scene::Update (where Unity sends them).
     *
     * Each frame it finds the target: first the UI hit provider (none until UI exists), then the world hit
     * provider, which by default casts the main camera's ScreenPointToRay against physics
     * (Raycast::Single with the pick mask, Layers::DefaultRaycastMask unless changed, so Ignore Raycast can't
     * be clicked; the target is the hit collider's GameObject). Without a physics backend nothing is hit.
     * The object that got OnMouseDown captures the pointer: it gets OnMouseDrag every frame the button stays
     * held and OnMouseUp on release wherever the pointer is, plus OnMouseUpAsButton first if released over it.
     *
     * Delivery follows the physics event rule: every component of an object active in the hierarchy (disabled
     * components included), none of an inactive or destroyed one. A hovered or capturing object that is
     * destroyed or deactivated is dropped without OnMouseExit or OnMouseUp. Main thread only.
     */
    class PointerDispatcher
    {
    public:
        /// The object under a screen point (window coordinates), or nullptr. It must be owned by a shared_ptr
        /// (every scene object is); anything else is treated as no hit.
        using HitProvider = std::function<GameObject *(const Math::Vector2 &screenPosition)>;

        /// One frame from the application's mouse (Mouse::Get); does nothing without one (no window)
        void Update();
        /// One frame from the given pointer state (what Update does with the mouse's state; tests call it)
        void Process(const PointerState &state);

        /// For UI (#42): asked before the world, and its hit wins. nullptr removes it.
        void SetUIHitProvider(HitProvider provider) { _uiProvider = std::move(provider); }
        /// Replaces the physics pick (tests, or a host with its own camera and viewport); nullptr restores it
        void SetWorldHitProvider(HitProvider provider) { _worldProvider = std::move(provider); }

        /// Whether the UI hit provider reported an element under the pointer in the last frame processed.
        /// Always false until UI exists. Games ask it to keep clicks on UI from reaching gameplay input.
        [[nodiscard]] bool IsPointerOverUI() const { return _pointerOverUI; }

        /// The layers the default physics pick can hit (Layers::DefaultRaycastMask to start with)
        void SetPickMask(const uint32_t mask) { _pickMask = mask; }
        [[nodiscard]] uint32_t GetPickMask() const { return _pickMask; }

        /// The object the pointer was over in the last frame processed, or nullptr. Like the callbacks, it
        /// ignores an object destroyed or deactivated since.
        [[nodiscard]] GameObject* GetHovered() const;
        /// The object that got OnMouseDown and still has the button, or nullptr (same rule)
        [[nodiscard]] GameObject* GetCaptured() const;

        /// Forgets the hovered and capturing objects without sending them anything. Safe from a callback or a
        /// hit provider: the frame being processed then stops sending.
        void Reset();

        /// The physics pick: casts camera.ScreenPointToRay(screenPosition, viewport) from the near plane to
        /// the far plane against the layers in mask and returns the hit collider's GameObject, or nullptr.
        /// A point outside the viewport (0 <= x < width, 0 <= y < height) picks nothing.
        [[nodiscard]] static GameObject* PickWorld(const Camera &camera, const Math::Vector2 &screenPosition,
                                                   const Vector2i &viewport, uint32_t mask);

    private:
        std::weak_ptr<GameObject> _hovered;
        std::weak_ptr<GameObject> _captured;
        HitProvider _uiProvider;
        HitProvider _worldProvider;
        uint32_t _pickMask = Layers::DefaultRaycastMask;
        bool _pointerOverUI = false;
        // Set while a frame is processed: a callback that calls Process/Update again is ignored
        bool _processing = false;
        // Counts Reset() calls, so a Process under way can tell it was reset from a callback
        uint64_t _resetCount = 0;

        /// The default world provider: PickWorld with the main camera and the window's size
        [[nodiscard]] GameObject* PickWithMainCamera(const Math::Vector2 &screenPosition) const;
        /// Calls one OnMouse* callback on every component of the object, by the physics event rule
        static void Send(GameObject &gameObject, void (Component::*callback)());
    };
}
