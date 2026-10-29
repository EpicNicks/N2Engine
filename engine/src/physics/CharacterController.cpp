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
#include "engine/physics/Rigidbody.hpp"

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
        /// How much a backend may round the edges of a box (Jolt gives its boxes a convex radius of up to 0.05; PhysX
        /// keeps them sharp): the edge probes look past it
        constexpr float kEdgeRounding = 0.05f;
        /// The steepest rise per run the edge probes allow for (a slope limit above about 63 degrees probes as 63)
        constexpr float kMaxProbeSlope = 2.0f;

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

        /// The horizontal part of `v` as a unit vector; false when it has next to none
        bool HorizontalUnit(const Vector3 &v, Vector3 &out)
        {
            const float length = std::sqrt(v.x * v.x + v.z * v.z);
            if (!(length > 1.0e-4f) || !std::isfinite(length))
            {
                return false;
            }
            out = Vector3(v.x / length, 0.0f, v.z / length);
            return true;
        }

        /// Ground under a capsule contact: its normal and the height of the surface stood on
        struct Support
        {
            Vector3 normal = Vector3(0.0f, 1.0f, 0.0f);
            float height = 0.0f;
        };

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
                const float probeSlope = std::min(Math::Tan(settings.slopeLimit * kDegreesToRadians), kMaxProbeSlope);
                behindInset = skin;
                behindBand = skin * (probeSlope + 0.5f);
                beyondInset = skin + kEdgeRounding;
                beyondRise = beyondInset * probeSlope + kEdgeRounding;
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

            /// The walkable surface a downward ray finds at `contact` moved `inset` along the horizontal unit `side`,
            /// no higher than `rise` above the contact and no lower than `drop` below it. The ray starts a skin
            /// width above that window, so a ray that starts inside a collider (distance 0) is never in it.
            bool WalkableSurfaceAt(const Vector3 &contact, const Vector3 &side, const float inset, const float rise,
                                   const float drop, Support &surface) const
            {
                const float lift = rise + skin;
                const Vector3 origin(contact.x + side.x * inset, contact.y + lift, contact.z + side.z * inset);
                RaycastHit hit;
                if (!backend.Raycast(origin, Vector3(0.0f, -1.0f, 0.0f), hit, lift + drop, layerMask,
                                     QueryTriggers::Ignore))
                {
                    return false;
                }
                if (!(hit.distance >= skin))
                {
                    return false;
                }
                const Vector3 normal = UnitOr(hit.normal, Vector3(0.0f, 1.0f, 0.0f));
                if (normal.y < minGroundY)
                {
                    return false;
                }
                surface.normal = normal;
                surface.height = origin.y - hit.distance;
                return true;
            }

            /// Whether the horizontal slide may climb along a contact: it is within the slope limit and the surface
            /// goes on just behind it (toward the capsule) at about its height: a floor, a ramp, the crest of a ramp.
            /// The rim of a step is not: behind it is the open face of the riser. A rim is a wall to the slide, so
            /// only the step-up climbs it and stepOffset is the limit, whatever the slanted normal a capsule's round
            /// end reports on it.
            bool IsClimbable(const RaycastHit &contact, const Vector3 &normal) const
            {
                if (normal.y < minGroundY)
                {
                    return false;
                }
                Vector3 towardCapsule;
                if (normal.y >= 1.0f - kGroundTolerance || !HorizontalUnit(normal, towardCapsule))
                {
                    return true;
                }
                Support behind;
                return WalkableSurfaceAt(contact.point, towardCapsule, behindInset, behindBand, behindBand, behind);
            }

            /// Whether a capsule contact below the capsule is ground to stand on: a contact within the slope limit,
            /// or (with `rims`) the rim of a walkable surface, where the capsule's round end rests on an edge and
            /// reports a slanted normal: just beyond the contact (away from the capsule) a downward ray finds ground
            /// within the slope limit, no lower than the contact and no higher than the edge's rounding and the
            /// slope allow. `support` gets the normal and height of what is stood on (the surface beyond, for a rim).
            bool IsGround(const RaycastHit &contact, const Vector3 &normal, const bool rims, Support &support) const
            {
                if (normal.y >= minGroundY)
                {
                    support.normal = normal;
                    support.height = contact.point.y;
                    return true;
                }
                Vector3 towardCapsule;
                if (!rims || !(normal.y > 0.0f) || !HorizontalUnit(normal, towardCapsule))
                {
                    return false;
                }
                const Vector3 away(-towardCapsule.x, 0.0f, -towardCapsule.z);
                return WalkableSurfaceAt(contact.point, away, beyondInset, beyondRise, skin, support);
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
            /// The rim probe behind a contact: how far in, and how far above or below the contact the surface may be
            float behindInset = 0.0f;
            float behindBand = 0.0f;
            /// The rim probe beyond a contact: how far out (past a rounded edge), and how far above the contact the
            /// surface may be
            float beyondInset = 0.0f;
            float beyondRise = 0.0f;

        private:
            mutable std::vector<OverlapHit> scratch;
        };

        enum class SlideKind
        {
            /// A horizontal move: climbs along what IsClimbable allows, slides along the rest as walls
            Horizontal,
            /// A vertical move in the air (a fall, a rise): stops on ground within the slope limit
            Air,
            /// The move down that keeps a standing character on the ground: stops on ground within the slope limit
            /// and on the rim of walkable ground
            Snap
        };

        struct SlideResult
        {
            Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
            /// Vertical move only: stopped by ground that can be stood on
            bool hitWalkable = false;
            Vector3 walkableNormal = Vector3(0.0f, 1.0f, 0.0f);
            /// Vertical move only: the contact that stopped it (what the character stands on)
            RaycastHit walkableHit;
            /// Vertical move only: stopped while going up
            bool hitCeiling = false;
            /// Horizontal move only: met a surface steeper than the slope limit, or the rim of a step (a wall)
            bool hitWall = false;
        };

        /// Moves the capsule by `delta`, stopping `skin` short of what it hits and bending the rest of the move along
        /// the surface (at most kSlideIterations times). The horizontal move climbs only what IsClimbable allows and
        /// slides along everything else as a wall, in the horizontal plane; the vertical moves stop on ground (see
        /// SlideKind) and slide down whatever else they meet.
        SlideResult Slide(const Context &c, const Vector3 &start, const Vector3 &delta, const SlideKind kind)
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
                remaining = direction * (length - travel);
                if (kind == SlideKind::Horizontal)
                {
                    Vector3 plane = normal;
                    if (!c.IsClimbable(hit, normal))
                    {
                        result.hitWall = true;
                        plane = UnitOr(Vector3(normal.x, 0.0f, normal.z), Vector3(-direction.x, 0.0f, -direction.z));
                    }
                    remaining = remaining - plane * Dot3(remaining, plane);
                }
                else
                {
                    Support support;
                    if (c.IsGround(hit, normal, kind == SlideKind::Snap, support))
                    {
                        result.hitWalkable = true;
                        result.walkableNormal = support.normal;
                        result.walkableHit = hit;
                        break;
                    }
                    if (direction.y > 0.0f)
                    {
                        result.hitCeiling = true;
                    }
                    remaining = remaining - normal * Dot3(remaining, normal);
                }
            }
            return result;
        }

        /// Climbs a step: up by the step offset (less under a ceiling), the horizontal move from there, and a sweep
        /// back down by as much. It is taken only when it gets further than walking into the obstacle did
        /// (`flatMoved`) and the sweep down ends on ground (IsGround, rims included: a capsule's round end resting
        /// on the edge of the step, which the next ticks climb on over) that is no higher than stepOffset above the
        /// bottom of the capsule where it started. That last test is what refuses the edge of anything taller: the
        /// raised capsule can pass over its rim and come down onto it, but what it would stand on is too high.
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
            const SlideResult over = Slide(c, raised, horizontalDelta, SlideKind::Horizontal);
            if (!(HorizontalDistance(over.position, start) > flatMoved + kStepProgressGain))
            {
                return false;
            }
            if (!c.Cast(over.position, down, raise + c.skin, hit))
            {
                return false;
            }
            Support support;
            if (!c.IsGround(hit, UnitOr(hit.normal, up), true, support))
            {
                return false;
            }
            const float startBottom = start.y - c.halfSegment - c.radius;
            if (!(support.height <= startBottom + c.stepOffset))
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
                                        const CharacterState &state, const CharacterInput &input, const float dt,
                                        RaycastHit *const groundHit)
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
            const SlideResult flat = Slide(c, position, horizontalDelta, SlideKind::Horizontal);
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
        RaycastHit contact;
        Vector3 groundNormal(0.0f, 1.0f, 0.0f);
        const float verticalDelta = vy * dt;
        if (std::abs(verticalDelta) > kMinMove)
        {
            const SlideResult vertical = Slide(c, position, Vector3(0.0f, verticalDelta, 0.0f), SlideKind::Air);
            position = vertical.position;
            if (vertical.hitWalkable)
            {
                landed = true;
                contact = vertical.walkableHit;
                groundNormal = vertical.walkableNormal;
                vy = 0.0f;
            }
            else if (vertical.hitCeiling && vy > 0.0f)
            {
                vy = 0.0f;
            }
        }

        // Ground: standing, it sticks to the ground (down a slope, down a step, round the edge it stands on and down
        // onto what is below, when that is no deeper than the step offset: a vertical slide that also stands on the
        // rim of walkable ground); coming down, it finds ground within the slope limit just below
        if (!landed && vy <= 0.0f)
        {
            if (wasGrounded && !jumped)
            {
                const SlideResult snap =
                    Slide(c, position, Vector3(0.0f, -(c.stepOffset + c.probe), 0.0f), SlideKind::Snap);
                if (snap.hitWalkable)
                {
                    position = snap.position;
                    landed = true;
                    contact = snap.walkableHit;
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
                        contact = hit;
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
        if (landed && groundHit != nullptr)
        {
            *groundHit = contact;
        }
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
        if (state.ground != 0)
        {
            // (Only a riding state: a state in the world frame hashes as it always did)
            mixByte(state.ground);
            mixByte(state.ground >> 8);
            mixByte(state.ground >> 16);
            mixByte(state.ground >> 24);
            mixFloat(state.groundLocal.x);
            mixFloat(state.groundLocal.y);
            mixFloat(state.groundLocal.z);
        }
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
        // The replication layer's cache of the state (netcode stage B6), used only on an object that has a NetworkObject
        RegisterMember("netPosition", _netPosition).Replicated(0.001).Hidden().ReadOnly();
        RegisterMember("netVelocityY", _netVelocityY).Replicated(0.001).Hidden().ReadOnly();
        RegisterMember("netGrounded", _netGrounded).Replicated().Hidden().ReadOnly();
        RegisterMember("netGround", _netGround).Replicated().Hidden().ReadOnly();
        RegisterMember("netGroundLocal", _netGroundLocal).Replicated(0.001).Hidden().ReadOnly();
    }

    nlohmann::json CharacterController::SerializeReplicated(const std::span<const FieldInfo> /*replicated*/)
    {
        _netPosition = _state.position;
        _netVelocityY = _state.velocity.y;
        _netGrounded = _state.grounded;
        _netGround = static_cast<int>(_state.ground);
        _netGroundLocal = _state.ground != 0 ? _state.groundLocal : Vector3(0.0f, 0.0f, 0.0f);
        return Serialize();
    }

    CharacterState CharacterController::GetReplicatedState() const
    {
        CharacterState state;
        state.position = _netPosition;
        state.velocity = Vector3(0.0f, _netVelocityY, 0.0f);
        state.grounded = _netGrounded;
        state.ground = _netGround > 0 ? static_cast<std::uint32_t>(_netGround) : 0u;
        state.groundLocal = state.ground != 0 ? _netGroundLocal : Vector3(0.0f, 0.0f, 0.0f);
        return state;
    }

    namespace
    {
        /// The pose the physics scene has for the kinematic body of `object`; false for any other object
        bool KinematicPose(IPhysicsBackend &backend, GameObject &object, Vector3 &position, Math::Quaternion &rotation)
        {
            const Rigidbody *const body = object.GetComponent<Rigidbody>();
            if (body == nullptr || !body->IsInitialized() || body->GetBodyType() != BodyType::Kinematic ||
                !object.IsActiveInHierarchy())
            {
                return false;
            }
            position = backend.GetPosition(body->GetHandle());
            rotation = backend.GetRotation(body->GetHandle());
            return IsFinite3(position);
        }
    }

    bool CharacterController::CanResolveGround(const std::uint32_t key) const
    {
        IPhysicsBackend *const backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend == nullptr || _groundMap == nullptr || key == 0)
        {
            return false;
        }
        GameObject *const object = _groundMap->BodyOf(key);
        Vector3 position;
        Math::Quaternion rotation;
        return object != nullptr && KinematicPose(*backend, *object, position, rotation);
    }

    CharacterState CharacterController::Anchored(const CharacterState &state) const
    {
        if (state.ground == 0)
        {
            return state;
        }
        CharacterState out = state;
        IPhysicsBackend *const backend = Application::GetInstance().Get3DPhysicsBackend();
        GameObject *const object = (backend != nullptr && _groundMap != nullptr) ? _groundMap->BodyOf(state.ground) : nullptr;
        Vector3 position;
        Math::Quaternion rotation;
        if (object != nullptr && KinematicPose(*backend, *object, position, rotation) && IsFinite3(state.groundLocal))
        {
            out.position = position + rotation * state.groundLocal;
            if (IsFinite3(out.position))
            {
                return out;
            }
        }
        // Not here (or not usable): the world frame, from the position the state holds
        out.ground = 0;
        out.groundLocal = Vector3(0.0f, 0.0f, 0.0f);
        return out;
    }

    CharacterState CharacterController::Simulate(const CharacterState &state, const CharacterInput &input, const float dt) const
    {
        IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend();
        const CharacterSettings settings = GetSettings();
        if (!backend || !settings.IsValid())
        {
            return state;
        }
        // Carried by the body it stood on: the step starts where that body (as this machine has it now) puts the offset
        const CharacterState start = Anchored(state);
        RaycastHit contact;
        CharacterState next = CharacterMotor::Step(*backend, settings, start, input, dt, &contact);
        next.ground = 0;
        next.groundLocal = Vector3(0.0f, 0.0f, 0.0f);
        if (next.grounded && _groundMap != nullptr && contact.rigidbody != nullptr)
        {
            GameObject &body = contact.rigidbody->GetGameObject();
            Vector3 position;
            Math::Quaternion rotation;
            if (KinematicPose(*backend, body, position, rotation))
            {
                const std::uint32_t key = _groundMap->KeyOf(body);
                if (key != 0)
                {
                    next.ground = key;
                    next.groundLocal = rotation.Conjugate() * (next.position - position);
                }
            }
        }
        return next;
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
        if (_externallyDriven)
        {
            return;
        }
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
        if (!IsFinite3(state.position) || !IsFinite3(state.velocity) || !IsFinite3(state.groundNormal) ||
            !IsFinite3(state.groundLocal))
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

        const CharacterState next = Simulate(_state, _input, dt);
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
