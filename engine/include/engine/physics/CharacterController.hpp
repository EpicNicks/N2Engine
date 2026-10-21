#pragma once

#include <cstdint>
#include <string>

#include <math/Vector3.hpp>

#include "engine/Layers.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;

    /// What the player asks of a character for one fixed tick
    struct CharacterInput
    {
        /// The wanted horizontal velocity in world units per second (the y component is ignored). A non-finite
        /// vector counts as zero and a longer one than CharacterMotor::kMaxMoveSpeed is cut to it.
        Math::Vector3 move = Math::Vector3(0.0f, 0.0f, 0.0f);
        /// Jump on this tick (only a grounded character jumps)
        bool jump = false;
    };

    /// Everything a character's next step depends on, and what the step reports. Copying it saves it and writing it
    /// back (CharacterController::SetState) restores it; a server correction is such a write. Step reads only
    /// position, velocity.y and grounded; velocity.x, velocity.z and groundNormal are results.
    struct CharacterState
    {
        /// The centre of the capsule, world space
        Math::Vector3 position = Math::Vector3(0.0f, 0.0f, 0.0f);
        /// x and z: the horizontal velocity the last step achieved (the distance it moved over its dt). y: the
        /// vertical velocity (jump and gravity), which the next step continues from.
        Math::Vector3 velocity = Math::Vector3(0.0f, 0.0f, 0.0f);
        /// Standing on ground that is within the slope limit
        bool grounded = false;
        /// The normal of the ground stood on, (0, 1, 0) when not grounded
        Math::Vector3 groundNormal = Math::Vector3(0.0f, 1.0f, 0.0f);
    };

    /// The shape and the tuning of a character
    struct CharacterSettings
    {
        /// The capsule's radius
        float radius = 0.5f;
        /// The capsule's whole height, caps included (as a CapsuleCollider's); at least 2 * radius (a shorter one is a
        /// sphere)
        float height = 2.0f;
        /// The tallest step it walks up
        float stepOffset = 0.3f;
        /// The steepest ground it stands on or walks up, in degrees from flat (0 to 89)
        float slopeLimit = 45.0f;
        /// The gap kept between the capsule and what it touches (cut to half the radius for a tiny capsule)
        float skinWidth = 0.02f;
        /// The upward speed a jump sets
        float jumpSpeed = 5.0f;
        /// Multiplies the physics gravity's y
        float gravityScale = 1.0f;
        /// The fastest it falls
        float maxFallSpeed = 50.0f;
        /// The layers it collides with (Layers::MaskOf, Layers::GetMask). Its own object's colliders must be left
        /// out: the capsule is a query shape and does not exclude itself.
        std::uint32_t layerMask = Layers::DefaultRaycastMask;

        /// Every value usable: finite, with the ranges above, a positive radius and skin width
        [[nodiscard]] bool IsValid() const;
    };

    /// The movement step as a function: (backend scene, settings, state, input, dt) -> the next state. No component,
    /// no hidden state, nothing read but its arguments and the backend's scene queries (capsule casts, capsule
    /// overlaps and the gravity), so the same arguments and scene give the same state: what client prediction replays.
    /// Float arithmetic only (no Vector3 dot, length or normalise, which dispatch on the CPU's SIMD), sqrt and the
    /// engine's Math::Cos; a fixed number of query calls at most.
    class CharacterMotor
    {
    public:
        /// The longest horizontal speed an input is taken at
        static constexpr float kMaxMoveSpeed = 1000.0f;

        /// One step of `dt` seconds. A state with a non-finite position, a dt that is not above zero and finite, or
        /// settings that are not valid return `state` unchanged.
        [[nodiscard]] static CharacterState Step(IPhysicsBackend &backend, const CharacterSettings &settings,
                                                 const CharacterState &state, const CharacterInput &input, float dt);

        /// A 64-bit FNV-1a hash over the bits of every field of the state, for comparing runs
        [[nodiscard]] static std::uint64_t Hash(const CharacterState &state);
    };

    /**
     * An engine-owned character controller (netcode B5): an upright capsule moved by CharacterMotor::Step once per
     * fixed tick, over the physics backend's capsule casts and overlaps, so it runs the same on PhysX and Jolt. It has
     * no body of its own: it is a query shape, other bodies do not see it, and it moves the object's transform (the
     * object is the only thing that follows the state). See docs/physics.html, "Character controller".
     *
     * The input is held: SetInput once and every tick uses it, except `jump`, which is cleared after the tick that
     * ran (a jump is one press). The state starts at the object's position when the component attaches (unless
     * SetState came first). The transform is an output: moving the object by hand does not move the character, use
     * Teleport or SetState.
     */
    class CharacterController final : public SerializableComponent
    {
    public:
        explicit CharacterController(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "CharacterController"; }

        void OnAttach() override;
        void OnFixedUpdate() override;

        static constexpr bool IsSingleton = true;

        /// The input of the next ticks (see the class comment). A non-finite move is refused (false).
        bool SetInput(const CharacterInput &input);
        [[nodiscard]] const CharacterInput &GetInput() const { return _input; }

        [[nodiscard]] const CharacterState &GetState() const { return _state; }
        /// Restores a state (what a correction or a rollback does) and moves the object to its position. False,
        /// changing nothing, for a non-finite position, velocity or ground normal. A ground normal of no length
        /// becomes (0, 1, 0).
        bool SetState(const CharacterState &state);
        /// The state at `position`, at rest and not grounded (the next step finds the ground)
        bool Teleport(const Math::Vector3 &position);

        /// One step with the current input and `dt`, now; the object moves to the new position. False when it did
        /// not run: no physics backend, settings that are not valid, or a dt that is not above zero and finite.
        bool Step(float dt);

        [[nodiscard]] CharacterSettings GetSettings() const;

        // Settings, all saved with the scene (the editor edits them; each is read at every step)

        /// The capsule's radius
        float radius = 0.5f;
        /// The capsule's whole height, caps included
        float height = 2.0f;
        /// The tallest step it walks up
        float stepOffset = 0.3f;
        /// The steepest ground it stands on or walks up, degrees from flat
        float slopeLimit = 45.0f;
        /// The gap kept between the capsule and what it touches
        float skinWidth = 0.02f;
        /// The upward speed of a jump
        float jumpSpeed = 5.0f;
        /// Multiplies the physics gravity's y
        float gravityScale = 1.0f;
        /// The fastest it falls
        float maxFallSpeed = 50.0f;
        /// The layers it collides with, as a bit mask; -1 is every layer (kept as an int so a scene file can hold it)
        int layerMask = static_cast<int>(Layers::DefaultRaycastMask);

    private:
        CharacterInput _input;
        CharacterState _state;
        bool _stateKnown = false;
        bool _warnedInvalid = false;
    };
}
