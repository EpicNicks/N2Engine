#include "engine/physics/CharacterController.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <math/DeterministicMath.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2Engine::Physics
{
    namespace
    {
        using Math::Vector3;

        constexpr float kDegreesToRadians = 0.017453292519943295f;
        /// A move shorter than this is not made (and no query is asked for it)
        constexpr float kMinMove = 1.0e-6f;
        /// A slide bends along at most this many surfaces in one move
        constexpr int kSlideIterations = 4;
        /// Penetration recovery looks this many rings out, each a step further, in 10 directions: at most 160 overlaps
        constexpr int kRecoveryRings = 16;
        /// A step is taken only when it gets further than walking into the obstacle did, by more than float noise
        constexpr float kStepProgressGain = 1.0e-6f;
        /// The slope test's tolerance: flat ground reports a normal a hair short of (0, 1, 0)
        constexpr float kGroundTolerance = 1.0e-3f;

        // Plain float arithmetic on the components: Vector3::Dot, Length and Normalized dispatch on the CPU's SIMD
        // support, so their last bits are not the same everywhere
        float Dot3(const Vector3 &a, const Vector3 &b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        float Length3(const Vector3 &v)
        {
            return std::sqrt(Dot3(v, v));
        }

        bool IsFinite3(const Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        float HorizontalDistance(const Vector3 &a, const Vector3 &b)
        {
            const float dx = a.x - b.x;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dz * dz);
        }

        /// `v` as a unit vector, or `fallback` for one that is not finite or has no length
        Vector3 UnitOr(const Vector3 &v, const Vector3 &fallback)
        {
            const float length = Length3(v);
            if (!(length > 1.0e-6f) || !std::isfinite(length))
            {
                return fallback;
            }
            return Vector3(v.x / length, v.y / length, v.z / length);
        }

        /// The capsule and the scene it is asked about, for one step
        class Context
        {
        public:
            Context(IPhysicsBackend &backendRef, const CharacterSettings &settings)
                : backend(backendRef),
                  radius(settings.radius),
                  halfSegment(std::max(0.0f, settings.height * 0.5f - settings.radius)),
                  skin(std::min(settings.skinWidth, settings.radius * 0.5f)),
                  probe(3.0f * std::min(settings.skinWidth, settings.radius * 0.5f)),
                  stepOffset(settings.stepOffset),
                  minGroundY(Math::Cos(settings.slopeLimit * kDegreesToRadians) - kGroundTolerance),
                  layerMask(settings.layerMask)
            {
            }

            /// Sweeps the capsule from `center` along the unit `direction`; false for no hit within `distance`
            bool Cast(const Vector3 &center, const Vector3 &direction, const float distance, RaycastHit &hit) const
            {
                const Vector3 point0(center.x, center.y - halfSegment, center.z);
                const Vector3 point1(center.x, center.y + halfSegment, center.z);
                return backend.CapsuleCast(point0, point1, radius, direction, hit, distance, layerMask,
                                           QueryTriggers::Ignore);
            }

            /// Whether the capsule at `center` overlaps a collider
            bool Overlaps(const Vector3 &center) const
            {
                OverlapShape shape;
                shape.kind = OverlapShape::Kind::Capsule;
                shape.point0 = Vector3(center.x, center.y - halfSegment, center.z);
                shape.point1 = Vector3(center.x, center.y + halfSegment, center.z);
                shape.radius = radius;
                return backend.Overlap(shape, scratch, layerMask, QueryTriggers::Ignore) > 0;
            }

            /// Whether a capsule contact with a ground-like normal is the edge of a step and not a slope: the surface
            /// just beyond the contact point (along the horizontal move `direction`) is flat while the contact normal
            /// is slanted (a capsule's round end meeting an edge reports a slanted normal at any step height)
            bool IsStepEdge(const RaycastHit &contact, const Vector3 &direction, const Vector3 &normal) const
            {
                if (normal.y >= 1.0f - kGroundTolerance)
                {
                    return false;
                }
                const Vector3 across = UnitOr(Vector3(direction.x, 0.0f, direction.z), Vector3(0.0f, 0.0f, 0.0f));
                if (across.x == 0.0f && across.z == 0.0f)
                {
                    return false;
                }
                const float inward = std::max(0.02f, skin);
                const float lift = std::max(0.05f, 2.0f * skin);
                const Vector3 origin(contact.point.x + across.x * inward, contact.point.y + lift,
                                     contact.point.z + across.z * inward);
                RaycastHit top;
                if (!backend.Raycast(origin, Vector3(0.0f, -1.0f, 0.0f), top, 2.0f * lift, layerMask,
                                     QueryTriggers::Ignore))
                {
                    return false;
                }
                if (UnitOr(top.normal, Vector3(0.0f, 1.0f, 0.0f)).y < 1.0f - kGroundTolerance)
                {
                    return false;
                }
                // The crest of a ramp that ends in a flat top is not a step: just behind the contact the surface is
                // still there, within the rise of any slope that can be stood on (behind the edge of a step it is
                // the open face of the riser, nothing within reach)
                const Vector3 behindOrigin(contact.point.x - across.x * inward, contact.point.y + lift,
                                           contact.point.z - across.z * inward);
                RaycastHit behind;
                if (backend.Raycast(behindOrigin, Vector3(0.0f, -1.0f, 0.0f), behind, 2.0f * lift, layerMask,
                                    QueryTriggers::Ignore) &&
                    behind.distance <= lift + 2.0f * inward &&
                    UnitOr(behind.normal, Vector3(0.0f, 1.0f, 0.0f)).y >= minGroundY)
                {
                    return false;
                }
                return true;
            }

            IPhysicsBackend &backend;
            float radius;
            /// Half the length of the segment between the centres of the end spheres
            float halfSegment;
            float skin;
            /// How far below the capsule counts as standing on something
            float probe;
            float stepOffset;
            /// The lowest normal.y of ground that can be stood on
            float minGroundY;
            std::uint32_t layerMask;

        private:
            mutable std::vector<OverlapHit> scratch;
        };

        struct SlideResult
        {
            Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
            /// Vertical move only: stopped by ground that can be stood on
            bool hitWalkable = false;
            Vector3 walkableNormal = Vector3(0.0f, 1.0f, 0.0f);
            /// Vertical move only: stopped while going up
            bool hitCeiling = false;
            /// Horizontal move only: met a surface steeper than the slope limit, or the edge of a step (a wall)
            bool hitWall = false;
        };

        /// Moves the capsule by `delta`, stopping `skin` short of what it hits and bending the rest of the move along
        /// the surface (at most kSlideIterations times). The horizontal move never climbs ground steeper than the slope
        /// limit (it slides along the wall, in the horizontal plane); the vertical move stops on ground that can be
        /// stood on and slides down ground that cannot.
        SlideResult Slide(const Context &c, const Vector3 &start, const Vector3 &delta, const bool vertical)
        {
            SlideResult result;
            result.position = start;
            Vector3 remaining = delta;
            for (int iteration = 0; iteration < kSlideIterations; ++iteration)
            {
                const float length = Length3(remaining);
                if (!(length > kMinMove))
                {
                    break;
                }
                const Vector3 direction(remaining.x / length, remaining.y / length, remaining.z / length);
                RaycastHit hit;
                if (!c.Cast(result.position, direction, length + c.skin, hit))
                {
                    result.position = result.position + remaining;
                    break;
                }
                const float allowed = hit.distance - c.skin;
                if (allowed >= length)
                {
                    result.position = result.position + remaining;
                    break;
                }
                const float travel = std::max(allowed, 0.0f);
                result.position = result.position + direction * travel;

                const Vector3 normal = UnitOr(hit.normal, Vector3(-direction.x, -direction.y, -direction.z));
                const bool walkable = normal.y >= c.minGroundY;
                remaining = direction * (length - travel);
                if (vertical)
                {
                    if (walkable)
                    {
                        result.hitWalkable = true;
                        result.walkableNormal = normal;
                        break;
                    }
                    if (direction.y > 0.0f)
                    {
                        result.hitCeiling = true;
                    }
                    remaining = remaining - normal * Dot3(remaining, normal);
                }
                else
                {
                    Vector3 plane = normal;
                    if (!walkable || c.IsStepEdge(hit, direction, normal))
                    {
                        result.hitWall = true;
                        plane = UnitOr(Vector3(normal.x, 0.0f, normal.z), Vector3(-direction.x, 0.0f, -direction.z));
                    }
                    remaining = remaining - plane * Dot3(remaining, plane);
                }
            }
            return result;
        }

        /// Climbs a step: up by the step height (less under a ceiling), the horizontal move from there, and back down
        /// onto what is below. False when that does not get further than walking into the obstacle did
        /// (`flatMoved`), or does not end on ground that can be stood on (a ledge, not a step). Once the move is known
        /// to clear the obstacle, the capsule is carried at least one radius forward, so that it ends over the top of
        /// the step and not balanced on its edge (where a capsule's contact normal is slanted and the ground would
        /// count as too steep).
        bool TryStepUp(const Context &c, const Vector3 &start, const Vector3 &horizontalDelta, const float flatMoved,
                       Vector3 &out)
        {
            const Vector3 up(0.0f, 1.0f, 0.0f);
            const Vector3 down(0.0f, -1.0f, 0.0f);
            float raise = c.stepOffset;
            RaycastHit hit;
            if (c.Cast(start, up, raise + c.skin, hit))
            {
                raise = std::max(hit.distance - c.skin, 0.0f);
            }
            if (!(raise > kMinMove))
            {
                return false;
            }
            const Vector3 raised(start.x, start.y + raise, start.z);
            const SlideResult clears = Slide(c, raised, horizontalDelta, false);
            if (!(HorizontalDistance(clears.position, start) > flatMoved + kStepProgressGain))
            {
                return false;
            }
            const float length = Length3(horizontalDelta);
            const float forward = std::max(length, c.radius);
            const SlideResult over = Slide(c, raised, horizontalDelta * (forward / length), false);
            if (!c.Cast(over.position, down, raise + c.skin, hit))
            {
                return false;
            }
            const Vector3 normal = UnitOr(hit.normal, up);
            if (normal.y < c.minGroundY)
            {
                return false;
            }
            // Perched on the edge of something taller than the step offset (a slanted contact, a flat top beyond):
            // that is not a landing
            const Vector3 heading(horizontalDelta.x / length, 0.0f, horizontalDelta.z / length);
            if (c.IsStepEdge(hit, heading, normal))
            {
                return false;
            }
            const float drop = std::max(hit.distance - c.skin, 0.0f);
            out = Vector3(over.position.x, over.position.y - drop, over.position.z);
            return true;
        }

        /// If the capsule overlaps a collider, the nearest free spot: up first, then the four sides, the four diagonals
        /// and down, ring after ring further out (a quarter radius, at least the skin width, per ring), and a skin
        /// width beyond that if it is free too. A capsule buried deeper than kRecoveryRings rings stays where it is.
        Vector3 Depenetrate(const Context &c, const Vector3 &position)
        {
            if (!c.Overlaps(position))
            {
                return position;
            }
            constexpr float kDiagonal = 0.70710678f;
            static constexpr float kDirections[10][3] = {
                {0.0f, 1.0f, 0.0f},          {1.0f, 0.0f, 0.0f},           {-1.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 1.0f},          {0.0f, 0.0f, -1.0f},          {kDiagonal, 0.0f, kDiagonal},
                {kDiagonal, 0.0f, -kDiagonal}, {-kDiagonal, 0.0f, kDiagonal}, {-kDiagonal, 0.0f, -kDiagonal},
                {0.0f, -1.0f, 0.0f}};
            const float ring = std::max(c.radius * 0.25f, c.skin);
            for (int i = 1; i <= kRecoveryRings; ++i)
            {
                const float distance = ring * static_cast<float>(i);
                for (const auto &d : kDirections)
                {
                    const Vector3 candidate(position.x + d[0] * distance, position.y + d[1] * distance,
                                            position.z + d[2] * distance);
                    if (c.Overlaps(candidate))
                    {
                        continue;
                    }
                    const Vector3 clear(candidate.x + d[0] * c.skin, candidate.y + d[1] * c.skin,
                                        candidate.z + d[2] * c.skin);
                    return c.Overlaps(clear) ? candidate : clear;
                }
            }
            return position;
        }
    }

    bool CharacterSettings::IsValid() const
    {
        return std::isfinite(radius) && radius > 0.0f && std::isfinite(height) && height > 0.0f &&
               std::isfinite(stepOffset) && stepOffset >= 0.0f && std::isfinite(slopeLimit) && slopeLimit >= 0.0f &&
               slopeLimit <= 89.0f && std::isfinite(skinWidth) && skinWidth > 0.0f && std::isfinite(jumpSpeed) &&
               jumpSpeed >= 0.0f && std::isfinite(gravityScale) && std::isfinite(maxFallSpeed) && maxFallSpeed > 0.0f;
    }

    CharacterState CharacterMotor::Step(IPhysicsBackend &backend, const CharacterSettings &settings,
                                        const CharacterState &state, const CharacterInput &input, const float dt)
    {
        if (!settings.IsValid() || !IsFinite3(state.position) || !(dt > 0.0f) || !std::isfinite(dt))
        {
            return state;
        }
        const Context c(backend, settings);

        // Out of anything the character was left inside (a teleport, a mover, a correction)
        const Vector3 basePosition = Depenetrate(c, state.position);
        Vector3 position = basePosition;

        // The wanted horizontal velocity: a non-finite one is none, a very long one is cut
        float moveX = input.move.x;
        float moveZ = input.move.z;
        if (!std::isfinite(moveX) || !std::isfinite(moveZ))
        {
            moveX = 0.0f;
            moveZ = 0.0f;
        }
        // (Cut by the larger part first, so that squaring a huge part below cannot overflow)
        const float largestPart = std::max(std::abs(moveX), std::abs(moveZ));
        if (largestPart > CharacterMotor::kMaxMoveSpeed)
        {
            const float scale = CharacterMotor::kMaxMoveSpeed / largestPart;
            moveX *= scale;
            moveZ *= scale;
        }
        const float moveSpeed = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (moveSpeed > CharacterMotor::kMaxMoveSpeed)
        {
            const float scale = CharacterMotor::kMaxMoveSpeed / moveSpeed;
            moveX *= scale;
            moveZ *= scale;
        }

        // Vertical velocity: standing has none (or a jump), falling accumulates gravity
        const bool wasGrounded = state.grounded;
        float vy = std::isfinite(state.velocity.y) ? state.velocity.y : 0.0f;
        bool jumped = false;
        if (wasGrounded)
        {
            vy = 0.0f;
            if (input.jump)
            {
                vy = settings.jumpSpeed;
                jumped = true;
            }
        }
        else
        {
            const float gravityY = backend.GetGravity().y;
            vy += gravityY * settings.gravityScale * dt;
        }
        if (!std::isfinite(vy))
        {
            vy = 0.0f;
        }
        vy = std::clamp(vy, -settings.maxFallSpeed, std::max(settings.jumpSpeed, settings.maxFallSpeed));

        // Horizontal move, with a step up when it is blocked and the character stands on something
        const Vector3 horizontalDelta(moveX * dt, 0.0f, moveZ * dt);
        const float horizontalLength = Length3(horizontalDelta);
        if (horizontalLength > kMinMove)
        {
            const SlideResult flat = Slide(c, position, horizontalDelta, false);
            const float flatMoved = HorizontalDistance(flat.position, position);
            Vector3 moved = flat.position;
            if (wasGrounded && !jumped && flat.hitWall && c.stepOffset > 0.0f)
            {
                Vector3 stepped = moved;
                if (TryStepUp(c, position, horizontalDelta, flatMoved, stepped))
                {
                    moved = stepped;
                }
            }
            position = moved;
        }

        // Vertical move: stops on ground it can stand on, or under a ceiling
        bool landed = false;
        Vector3 groundNormal(0.0f, 1.0f, 0.0f);
        const float verticalDelta = vy * dt;
        if (std::abs(verticalDelta) > kMinMove)
        {
            const SlideResult vertical = Slide(c, position, Vector3(0.0f, verticalDelta, 0.0f), true);
            position = vertical.position;
            if (vertical.hitWalkable)
            {
                landed = true;
                groundNormal = vertical.walkableNormal;
                vy = 0.0f;
            }
            else if (vertical.hitCeiling && vy > 0.0f)
            {
                vy = 0.0f;
            }
        }

        // Ground: standing, it sticks to the ground (down a slope, down a step, off the edge of a ledge no deeper than
        // the step offset: a vertical slide, which slides off a slanted edge and ends on what is below it); coming
        // down, it finds the ground just below
        if (!landed && vy <= 0.0f)
        {
            if (wasGrounded && !jumped)
            {
                const SlideResult snap =
                    Slide(c, position, Vector3(0.0f, -(c.stepOffset + c.probe), 0.0f), true);
                if (snap.hitWalkable)
                {
                    position = snap.position;
                    landed = true;
                    groundNormal = snap.walkableNormal;
                    vy = 0.0f;
                }
            }
            else
            {
                RaycastHit hit;
                if (c.Cast(position, Vector3(0.0f, -1.0f, 0.0f), c.probe, hit))
                {
                    const Vector3 normal = UnitOr(hit.normal, Vector3(0.0f, 1.0f, 0.0f));
                    if (normal.y >= c.minGroundY)
                    {
                        const float drop = std::max(hit.distance - c.skin, 0.0f);
                        position = Vector3(position.x, position.y - drop, position.z);
                        landed = true;
                        groundNormal = normal;
                        vy = 0.0f;
                    }
                }
            }
        }

        if (!IsFinite3(position))
        {
            return state;
        }
        CharacterState next;
        next.position = position;
        next.grounded = landed;
        next.groundNormal = landed ? groundNormal : Vector3(0.0f, 1.0f, 0.0f);
        float achievedX = (position.x - basePosition.x) / dt;
        float achievedZ = (position.z - basePosition.z) / dt;
        if (!std::isfinite(achievedX) || !std::isfinite(achievedZ))
        {
            achievedX = 0.0f;
            achievedZ = 0.0f;
        }
        next.velocity = Vector3(achievedX, vy, achievedZ);
        return next;
    }

    std::uint64_t CharacterMotor::Hash(const CharacterState &state)
    {
        std::uint64_t hash = 14695981039346656037ull;
        const auto mixByte = [&hash](const std::uint32_t byte)
        {
            hash ^= static_cast<std::uint64_t>(byte & 0xFFu);
            hash *= 1099511628211ull;
        };
        const auto mixFloat = [&mixByte](const float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            mixByte(bits);
            mixByte(bits >> 8);
            mixByte(bits >> 16);
            mixByte(bits >> 24);
        };
        mixFloat(state.position.x);
        mixFloat(state.position.y);
        mixFloat(state.position.z);
        mixFloat(state.velocity.x);
        mixFloat(state.velocity.y);
        mixFloat(state.velocity.z);
        mixFloat(state.groundNormal.x);
        mixFloat(state.groundNormal.y);
        mixFloat(state.groundNormal.z);
        mixByte(state.grounded ? 1u : 0u);
        return hash;
    }

    CharacterController::CharacterController(GameObject &gameObject) : SerializableComponent(gameObject)
    {
        _gameObject.CreatePositionable();

        RegisterMember(NAMEOF(radius), radius).Range(0.01, 100.0).Tooltip("The capsule's radius");
        RegisterMember(NAMEOF(height), height).Range(0.02, 1000.0)
            .Tooltip("The capsule's whole height, caps included (at least twice the radius)");
        RegisterMember(NAMEOF(stepOffset), stepOffset).Range(0.0, 100.0).Tooltip("The tallest step it walks up");
        RegisterMember(NAMEOF(slopeLimit), slopeLimit).Range(0.0, 89.0)
            .Tooltip("The steepest ground it stands on or walks up, in degrees from flat");
        RegisterMember(NAMEOF(skinWidth), skinWidth).Range(0.0001, 1.0)
            .Tooltip("The gap kept between the capsule and what it touches (at most half the radius)");
        RegisterMember(NAMEOF(jumpSpeed), jumpSpeed).Range(0.0, 1000.0).Tooltip("The upward speed a jump sets");
        RegisterMember(NAMEOF(gravityScale), gravityScale).Range(-100.0, 100.0)
            .Tooltip("Multiplies the physics gravity's y");
        RegisterMember(NAMEOF(maxFallSpeed), maxFallSpeed).Range(0.01, 100000.0).Tooltip("The fastest it falls");
        RegisterMember(NAMEOF(layerMask), layerMask)
            .Tooltip("The layers it collides with, as a bit mask (-1 is every layer). Leave out the layer of its own colliders.");
    }

    void CharacterController::OnAttach()
    {
        if (_stateKnown)
        {
            return;
        }
        if (const Positionable *positionable = GetGameObject().GetPositionable())
        {
            _state = CharacterState{};
            _state.position = positionable->GetPosition();
            _stateKnown = true;
        }
    }

    void CharacterController::OnFixedUpdate()
    {
        // The scaled fixed step: 0 while paused (time scale 0), so nothing moves
        Step(Time::GetFixedDeltaTime());
    }

    CharacterSettings CharacterController::GetSettings() const
    {
        CharacterSettings settings;
        settings.radius = radius;
        settings.height = height;
        settings.stepOffset = stepOffset;
        settings.slopeLimit = slopeLimit;
        settings.skinWidth = skinWidth;
        settings.jumpSpeed = jumpSpeed;
        settings.gravityScale = gravityScale;
        settings.maxFallSpeed = maxFallSpeed;
        settings.layerMask = static_cast<std::uint32_t>(layerMask);
        return settings;
    }

    bool CharacterController::SetInput(const CharacterInput &input)
    {
        if (!IsFinite3(input.move))
        {
            return false;
        }
        _input = input;
        return true;
    }

    bool CharacterController::SetState(const CharacterState &state)
    {
        if (!IsFinite3(state.position) || !IsFinite3(state.velocity) || !IsFinite3(state.groundNormal))
        {
            return false;
        }
        _state = state;
        _state.groundNormal = UnitOr(state.groundNormal, Vector3(0.0f, 1.0f, 0.0f));
        _stateKnown = true;
        if (Positionable *positionable = GetGameObject().GetPositionable())
        {
            positionable->SetPosition(_state.position);
        }
        return true;
    }

    bool CharacterController::Teleport(const Math::Vector3 &position)
    {
        CharacterState state;
        state.position = position;
        return SetState(state);
    }

    bool CharacterController::Step(const float dt)
    {
        IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend || !(dt > 0.0f) || !std::isfinite(dt))
        {
            return false;
        }
        const CharacterSettings settings = GetSettings();
        if (!settings.IsValid())
        {
            if (!_warnedInvalid)
            {
                _warnedInvalid = true;
                Logger::Warn("CharacterController settings are not valid (radius, skinWidth and maxFallSpeed above 0, "
                             "slopeLimit 0 to 89, the others finite and not negative): it does not move");
            }
            return false;
        }
        _warnedInvalid = false;
        if (!_stateKnown)
        {
            if (const Positionable *positionable = GetGameObject().GetPositionable())
            {
                _state = CharacterState{};
                _state.position = positionable->GetPosition();
                _stateKnown = true;
            }
        }

        const CharacterState next = CharacterMotor::Step(*backend, settings, _state, _input, dt);
        _input.jump = false; // a jump is one press
        const bool moved = next.position.x != _state.position.x || next.position.y != _state.position.y ||
                           next.position.z != _state.position.z;
        _state = next;
        if (moved)
        {
            if (Positionable *positionable = GetGameObject().GetPositionable())
            {
                positionable->SetPosition(_state.position);
            }
        }
        return true;
    }
}
