#include "engine/network/NetworkPrediction.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"

namespace N2Engine::Network
{
    using Physics::CharacterController;
    using Physics::CharacterInput;
    using Physics::CharacterMotor;
    using Physics::CharacterState;

    namespace
    {
        bool Finite3(const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        float Distance(const Math::Vector3 &a, const Math::Vector3 &b)
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /// The part of a server state a step depends on; what the step produces (horizontal velocity, ground normal) is
        /// left at rest
        CharacterState Normalised(const CharacterState &server)
        {
            CharacterState state;
            state.position = server.position;
            state.velocity = Math::Vector3(0.0f, server.velocity.y, 0.0f);
            state.grounded = server.grounded;
            state.ground = server.ground;
            state.groundLocal = server.ground != 0 ? server.groundLocal : Math::Vector3(0.0f, 0.0f, 0.0f);
            return state;
        }
    }

    // ---- CharacterInputMap -------------------------------------------------------------------------------------------

    std::expected<CharacterInputMap, std::string> CharacterInputMap::Create(const CharacterInputMapping &mapping,
                                                                            const InputLayout &layout)
    {
        CharacterInputMap map;
        map._valueCount = layout.ValueCount();
        map._moveSpeed = std::isfinite(mapping.moveSpeed)
                             ? std::clamp(mapping.moveSpeed, 0.0f, CharacterMotor::kMaxMoveSpeed)
                             : 0.0f;
        if (!mapping.moveControl.empty())
        {
            const std::optional<std::size_t> index = layout.FindControl(mapping.moveControl);
            if (!index.has_value())
            {
                return std::unexpected(std::format("the input layout has no control '{}' to move by", mapping.moveControl));
            }
            if (layout.Control(*index).kind != InputControlKind::Vector2)
            {
                return std::unexpected(std::format("the control '{}' moves the character, so it must be a vector2 (it is a {})",
                                                   mapping.moveControl, ToString(layout.Control(*index).kind)));
            }
            map._hasMove = true;
            map._moveOffset = layout.ValueOffset(*index);
        }
        if (!mapping.jumpControl.empty())
        {
            const std::optional<std::size_t> index = layout.FindControl(mapping.jumpControl);
            if (!index.has_value())
            {
                return std::unexpected(std::format("the input layout has no control '{}' to jump by", mapping.jumpControl));
            }
            if (layout.Control(*index).kind != InputControlKind::Button)
            {
                return std::unexpected(std::format("the control '{}' makes the character jump, so it must be a button (it is a {})",
                                                   mapping.jumpControl, ToString(layout.Control(*index).kind)));
            }
            map._hasJump = true;
            map._jumpOffset = layout.ValueOffset(*index);
        }
        return map;
    }

    CharacterInput CharacterInputMap::Map(const std::span<const float> values) const
    {
        CharacterInput input;
        if (values.size() < _valueCount)
        {
            return input;
        }
        if (_hasMove)
        {
            float x = values[_moveOffset];
            float y = values[_moveOffset + 1];
            x = std::isfinite(x) ? x : 0.0f;
            y = std::isfinite(y) ? y : 0.0f;
            const float lengthSquared = x * x + y * y;
            if (lengthSquared > 1.0f)
            {
                const float length = std::sqrt(lengthSquared);
                x /= length;
                y /= length;
            }
            input.move = Math::Vector3(x * _moveSpeed, 0.0f, y * _moveSpeed);
        }
        if (_hasJump)
        {
            input.jump = values[_jumpOffset] >= 0.5f;
        }
        return input;
    }

    // ---- PredictionOptions -------------------------------------------------------------------------------------------

    PredictionOptions PredictionOptions::Clamped() const noexcept
    {
        PredictionOptions out = *this;
        if (!(out.stepSeconds > 0.0f) || !std::isfinite(out.stepSeconds))
        {
            out.stepSeconds = 1.0f / 60.0f;
        }
        out.positionEpsilon = std::isfinite(out.positionEpsilon) ? std::max(out.positionEpsilon, 0.0f) : 0.01f;
        out.velocityEpsilon = std::isfinite(out.velocityEpsilon) ? std::max(out.velocityEpsilon, 0.0f) : 0.05f;
        out.snapDistance = std::isfinite(out.snapDistance) ? std::max(out.snapDistance, 0.0f) : 1.0f;
        out.smoothingMicros = std::min<std::uint64_t>(out.smoothingMicros, 5'000'000);
        out.historyTicks = std::clamp<std::uint32_t>(out.historyTicks, 8u, static_cast<std::uint32_t>(kMaxPredictionHistory));
        return out;
    }

    PredictionOptions PredictionOptions::FromSettings(const NetworkSettings &settings) noexcept
    {
        PredictionOptions out;
        out.stepSeconds = settings.tickRate > 0 ? 1.0f / static_cast<float>(settings.tickRate) : out.stepSeconds;
        out.smoothingMicros = static_cast<std::uint64_t>(std::max(settings.authoritative.smoothingMs, 0)) * 1000u;
        out.snapDistance = static_cast<float>(settings.authoritative.snapThreshold);
        return out.Clamped();
    }

    PredictionTuning PredictionTuning::Of(const PredictionOptions &options) noexcept
    {
        PredictionTuning tuning;
        tuning.positionEpsilon = options.positionEpsilon;
        tuning.velocityEpsilon = options.velocityEpsilon;
        tuning.snapDistance = options.snapDistance;
        tuning.smoothingMicros = options.smoothingMicros;
        tuning.applySmoothing = options.applySmoothing;
        return tuning;
    }

    PredictionOptions PredictionTuning::AppliedTo(const PredictionOptions &options) const noexcept
    {
        PredictionOptions out = options;
        out.positionEpsilon = positionEpsilon;
        out.velocityEpsilon = velocityEpsilon;
        out.snapDistance = snapDistance;
        out.smoothingMicros = smoothingMicros;
        out.applySmoothing = applySmoothing;
        return out.Clamped();
    }

    // ---- PredictionHistory -------------------------------------------------------------------------------------------

    PredictionHistory::PredictionHistory(const std::size_t capacity)
        : _capacity(std::clamp<std::size_t>(capacity, 8, kMaxPredictionHistory))
    {
    }

    void PredictionHistory::Clear() noexcept
    {
        _head = 0;
        _count = 0;
        _hasEvicted = false;
        _hasConfirmed = false;
    }

    void PredictionHistory::DropFront(const std::size_t count) noexcept
    {
        const std::size_t n = std::min(count, _count);
        _head = (_head + n) % _capacity;
        _count -= n;
    }

    bool PredictionHistory::Record(const std::uint32_t tick, const CharacterInput &input, const float dt,
                                   const CharacterState &after)
    {
        if (_count > 0 && TickDifference(tick, At(_count - 1).tick) <= 0)
        {
            return false;
        }
        if (_count == _capacity)
        {
            _hasEvicted = true;
            _evictedThrough = At(0).tick;
            DropFront(1);
        }
        Sample &sample = At(_count);
        sample.tick = tick;
        sample.dt = dt;
        sample.input = input;
        sample.state = after;
        ++_count;
        return true;
    }

    const CharacterState *PredictionHistory::NewestState() const noexcept
    {
        return _count == 0 ? nullptr : &At(_count - 1).state;
    }

    bool PredictionHistory::NewestTick(std::uint32_t &tick) const noexcept
    {
        if (_count == 0)
        {
            return false;
        }
        tick = At(_count - 1).tick;
        return true;
    }

    const CharacterState *PredictionHistory::StateAt(const std::uint32_t tick) const noexcept
    {
        for (std::size_t i = 0; i < _count; ++i)
        {
            if (At(i).tick == tick)
            {
                return &At(i).state;
            }
        }
        return nullptr;
    }

    ReconcileResult PredictionHistory::Reconcile(const std::uint32_t ackTick, const CharacterState &server,
                                                 const PredictionStepper &step, const PredictionOptions &options)
    {
        ReconcileResult result;
        if (!Finite3(server.position) || !std::isfinite(server.velocity.y) || !Finite3(server.groundLocal) || _count == 0)
        {
            return result;
        }
        // The server cannot have applied an input this client has not made
        if (TickDifference(ackTick, At(_count - 1).tick) > 0)
        {
            return result;
        }

        // Where is the tick?
        std::size_t found = _count;
        for (std::size_t i = 0; i < _count; ++i)
        {
            if (At(i).tick == ackTick)
            {
                found = i;
                break;
            }
        }

        std::size_t firstToReplay = 0;
        if (found < _count)
        {
            const CharacterState &predicted = At(found).state;
            result.compared = true;
            // Riding (wire version 8): two states on the same body are compared by their offsets in its local space, so
            // the body's own motion (and the lag of this machine's copy of it) is not an error; on different grounds
            // they are compared in the world frame (the caller has put the server's state in this machine's frame)
            const bool sameRide = predicted.ground != 0 && predicted.ground == server.ground;
            result.error = sameRide ? Distance(predicted.groundLocal, server.groundLocal)
                                    : Distance(predicted.position, server.position);
            const bool agrees = result.error <= options.positionEpsilon && predicted.ground == server.ground &&
                                std::fabs(predicted.velocity.y - server.velocity.y) <= options.velocityEpsilon &&
                                predicted.grounded == server.grounded;
            _hasConfirmed = true;
            _confirmedThrough = ackTick;
            if (agrees)
            {
                result.outcome = ReconcileOutcome::Match;
                result.state = predicted;
                DropFront(found + 1);
                return result;
            }
            firstToReplay = found + 1;
        }
        else
        {
            // Not in the history. Before its first sample: dropped because the history overflowed, confirmed already, or
            // the server applied the same input twice.
            if (TickDifference(ackTick, At(0).tick) > 0)
            {
                // Between two samples: a tick this client never stepped
                return result;
            }
            if (_hasEvicted && TickDifference(ackTick, _evictedThrough) < 0)
            {
                // The inputs between the server's tick and the oldest we hold are gone: nothing to replay from
                result.outcome = ReconcileOutcome::Overflow;
                result.state = Normalised(server);
                result.error = Distance(At(_count - 1).state.position, server.position);
                // Later acks that still name a tick before the newest we held are overflows too
                const std::uint32_t newest = At(_count - 1).tick;
                _count = 0;
                _head = 0;
                _hasEvicted = true;
                _evictedThrough = newest;
                return result;
            }
            if (_hasConfirmed && TickDifference(ackTick, _confirmedThrough) < 0)
            {
                // Older than a state already used
                return result;
            }
            firstToReplay = 0;
        }

        CharacterState state = Normalised(server);
        // The new current state: the server's with every input after its tick applied again
        for (std::size_t i = firstToReplay; i < _count; ++i)
        {
            Sample &sample = At(i);
            state = step(state, sample.input, sample.dt);
            sample.state = state;
            ++result.replayed;
        }
        if (found < _count)
        {
            DropFront(found + 1);
        }
        result.outcome = ReconcileOutcome::Corrected;
        result.state = state;
        return result;
    }

    // ---- The server's side -------------------------------------------------------------------------------------------

    ServerStepResult StepServerCharacter(NetworkServer &server, NetworkObject &object, const CharacterInputMap &map,
                                         const std::uint32_t tick, const float dt)
    {
        CharacterController *const controller = object.GetGameObject().GetComponent<CharacterController>();
        if (controller == nullptr)
        {
            return ServerStepResult::NoController;
        }
        controller->SetExternallyDriven(true);
        controller->SetGroundMap(&server.World());
        CharacterInput input;
        if (object.HasNetworkId())
        {
            const std::optional<ControlledInput> controlled = server.InputFor(object.GetNetworkId(), tick);
            if (controlled.has_value())
            {
                input = map.Map(controlled->values);
            }
        }
        (void)controller->SetInput(input);
        return controller->Step(dt) ? ServerStepResult::Stepped : ServerStepResult::NotRun;
    }
}
