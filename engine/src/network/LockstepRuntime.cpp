#include "engine/network/LockstepRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/network/LockstepInputMapping.hpp"
#include "engine/replay/GoldenReplay.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint64_t kAckIntervalMicros = 50'000;

        bool Ahead(const std::uint32_t a, const std::uint32_t b) noexcept
        {
            return static_cast<std::int32_t>(a - b) > 0;
        }
    }

    std::string_view ToString(const LockstepState state) noexcept
    {
        switch (state)
        {
        case LockstepState::Connecting:
            return "connecting";
        case LockstepState::Joining:
            return "joining";
        case LockstepState::Lobby:
            return "lobby";
        case LockstepState::Running:
            return "running";
        case LockstepState::Stopped:
            return "stopped";
        case LockstepState::Refused:
            return "refused";
        case LockstepState::Failed:
            return "failed";
        }
        return "unknown";
    }

    std::expected<InputLayout, std::string> ParseInputLayoutJson(const std::string_view text)
    {
        nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.is_array())
        {
            return std::unexpected("the layout is not a JSON list of controls");
        }
        if (json.size() > kMaxInputControls)
        {
            return std::unexpected(std::format("a layout holds at most {} controls", kMaxInputControls));
        }
        std::vector<InputControlSpec> specs;
        for (const nlohmann::json &entry : json)
        {
            if (!entry.is_object())
            {
                return std::unexpected("every control must be an object");
            }
            InputControlSpec spec;
            bool haveName = false;
            bool haveKind = false;
            for (const auto &[key, value] : entry.items())
            {
                if (key == "name" && value.is_string())
                {
                    spec.name = value.get<std::string>();
                    haveName = true;
                }
                else if (key == "kind" && value.is_string())
                {
                    const auto kind = ParseInputControlKind(value.get<std::string>());
                    if (!kind)
                    {
                        return std::unexpected("kind must be button, axis, vector2 or vector3");
                    }
                    spec.kind = *kind;
                    haveKind = true;
                }
                else if (key == "bits" && value.is_number_integer() && value.get<int>() >= 1 && value.get<int>() <= 255)
                {
                    spec.bits = static_cast<std::uint8_t>(value.get<int>());
                }
                else if (key == "range" && value.is_number())
                {
                    spec.range = value.get<float>();
                }
                else if (key == "signed" && value.is_boolean())
                {
                    spec.isSigned = value.get<bool>();
                }
                else
                {
                    return std::unexpected("a control has a field that is not name, kind, bits, range or signed, or one of the wrong type");
                }
            }
            if (!haveName || !haveKind)
            {
                return std::unexpected("every control needs a name and a kind");
            }
            specs.push_back(std::move(spec));
        }
        return InputLayout::Create(std::move(specs));
    }

#if !defined(__EMSCRIPTEN__)
    /// A body that runs on its own thread but only while the caller is inside Resume(): strict hand-off, so exactly one
    /// of the two runs at a time and the body may use the runtime freely. Yield() gives control back.
    struct LockstepRuntime::Coroutine
    {
        explicit Coroutine(std::function<void(Coroutine &)> bodyFunction) : body(std::move(bodyFunction)) {}

        ~Coroutine()
        {
            abort = true;
            while (!done && thread.joinable())
            {
                Resume();
            }
            if (thread.joinable())
            {
                thread.join();
            }
        }

        void Resume()
        {
            std::unique_lock lock(mutex);
            if (done)
            {
                return;
            }
            turn = Turn::Body;
            if (!thread.joinable())
            {
                thread = std::thread(
                    [this]
                    {
                        std::unique_lock inner(mutex);
                        cv.wait(inner, [this] { return turn == Turn::Body; });
                        inner.unlock();
                        body(*this);
                        inner.lock();
                        done = true;
                        turn = Turn::Caller;
                        cv.notify_all();
                    });
            }
            cv.notify_all();
            cv.wait(lock, [this] { return turn == Turn::Caller; });
        }

        /// Called by the body
        void Yield()
        {
            std::unique_lock lock(mutex);
            turn = Turn::Caller;
            cv.notify_all();
            cv.wait(lock, [this] { return turn == Turn::Body; });
        }

        enum class Turn : std::uint8_t
        {
            Caller,
            Body,
        };

        std::function<void(Coroutine &)> body;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable cv;
        Turn turn = Turn::Caller;
        bool done = false;
        bool abort = false;
    };
#else
    struct LockstepRuntime::Coroutine
    {
    };
#endif

    /// The driver the networked hash-tree peer waits on: a step yields to the caller, who pumps the network
    struct LockstepRuntime::DriverImpl final : N2Net::IBisectDriver
    {
        DriverImpl(LockstepRuntime &owner, Coroutine &coroutine) : runtime(owner), co(coroutine) {}

        [[nodiscard]] std::uint64_t NowMicros() const override { return runtime.now_; }

        bool Step() override
        {
#if !defined(__EMSCRIPTEN__)
            co.Yield();
            return !co.abort && runtime.connection_ != nullptr && !runtime.Finished();
#else
            return false;
#endif
        }

        LockstepRuntime &runtime;
        Coroutine &co;
    };

    LockstepRuntime::LockstepRuntime(N2Net::ITransport &transport, LockstepRuntimeOptions options)
        : transport_(transport), options_(std::move(options)), history_(options_.historyDepth)
    {
        clock_.SetMode(SimulationMode::Deterministic);
        // Lockstep paces itself: a tick that waits for an input resets the accumulator, so a "behind" report is not an
        // error here; with a handler installed the clock never drops time on its own
        clock_.SetBehindHandler([this](const SimulationBehind &) { ++stats_.framesBehind; });
    }

    LockstepRuntime::~LockstepRuntime()
    {
        // A bisection still waiting is aborted before the members it uses go
        bisection_.reset();
    }

    std::uint32_t LockstepRuntime::TicksRun() const noexcept
    {
        return scheduler_ ? scheduler_->CurrentTick() : 0;
    }

    void LockstepRuntime::Notify(const LockstepEvent::Type type, const std::uint32_t slot, const std::uint32_t tick,
                                 std::string text)
    {
        if (options_.observer)
        {
            options_.observer(LockstepEvent{type, slot, tick, std::move(text)});
        }
    }

    void LockstepRuntime::Fail(std::string text)
    {
        if (Finished())
        {
            return;
        }
        error_ = std::move(text);
        state_ = LockstepState::Failed;
        Notify(LockstepEvent::Type::Failed, slot_, TicksRun(), error_);
        if (server_ != N2Net::kInvalidPeer)
        {
            transport_.Disconnect(server_);
        }
    }

    void LockstepRuntime::SendReliable(const std::vector<std::uint8_t> &message)
    {
        if (connection_ && !message.empty() && connection_->SendReliable(message) != N2Net::SendResult::Ok)
        {
            Fail("the reliable send queue to the relay is full");
        }
    }

    void LockstepRuntime::SetPaused(const bool paused)
    {
        if (state_ == LockstepState::Running)
        {
            SendReliable(EncodePause(paused));
        }
    }

    void LockstepRuntime::Leave()
    {
        if (server_ != N2Net::kInvalidPeer)
        {
            transport_.Disconnect(server_);
        }
        if (!Finished())
        {
            state_ = LockstepState::Stopped;
        }
    }

    void LockstepRuntime::HandleEvent(const N2Net::TransportEvent &event, const std::uint64_t now)
    {
        switch (event.type)
        {
        case N2Net::TransportEventType::PeerConnected:
            if (server_ == N2Net::kInvalidPeer && state_ == LockstepState::Connecting)
            {
                server_ = event.peer;
                auto config = MakeLockstepConnectionConfig(options_.protocolId, transport_);
                config.reliableStream = transport_.IsReliablePeer(event.peer);
                config.maxPacketSize = static_cast<std::uint32_t>(
                    std::min(transport_.MaxPeerPacketSize(event.peer), N2Net::kMaxPacketSize));
                connection_ = std::make_unique<N2Net::Connection>(config, now);
                state_ = LockstepState::Joining;
                JoinMessage join;
                join.payloadBytes = static_cast<std::uint8_t>(options_.layout.PayloadBytes());
                join.layoutHash = options_.layout.Hash();
                SendReliable(Encode(join));
            }
            break;
        case N2Net::TransportEventType::PeerDisconnected:
            if (event.peer == server_ && !Finished())
            {
                if (pendingStop_ || stop_)
                {
                    state_ = LockstepState::Stopped;
                }
                else
                {
                    Fail("the relay closed the connection");
                }
            }
            break;
        case N2Net::TransportEventType::Datagram:
            if (event.peer == server_ && connection_)
            {
                const auto handler = [this, now](const N2Net::MessageChannel channel,
                                                 const std::span<const std::uint8_t> message)
                { OnMessage(channel, message, now); };
                (void)connection_->OnPacket(now, event.data, handler);
            }
            break;
        }
    }

    void LockstepRuntime::OnMessage(const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message,
                                    const std::uint64_t now)
    {
        if (message.empty() || Finished())
        {
            return;
        }
        if (channel == N2Net::MessageChannel::UnreliableSequenced)
        {
            if (message[0] == 1)
            {
                OnBundle(message);
            }
            return;
        }
        switch (message[0])
        {
        case static_cast<std::uint8_t>(RelayMessage::Welcome):
            if (state_ == LockstepState::Joining)
            {
                if (const auto welcome = DecodeWelcome(message))
                {
                    OnWelcome(*welcome);
                }
                else
                {
                    Fail("the relay sent a malformed welcome");
                }
            }
            break;
        case static_cast<std::uint8_t>(RelayMessage::Start):
            if (state_ == LockstepState::Lobby)
            {
                state_ = LockstepState::Running;
                Notify(LockstepEvent::Type::Started, slot_, 0, "");
            }
            break;
        case static_cast<std::uint8_t>(RelayMessage::Refuse):
            if (const auto reason = DecodeRefuse(message))
            {
                refusal_ = *reason;
                error_ = std::format("the relay refused the join ({})", ToString(*reason));
                state_ = LockstepState::Refused;
                Notify(LockstepEvent::Type::Refused, 0, 0, error_);
            }
            break;
        case static_cast<std::uint8_t>(RelayMessage::DropSlot):
        {
            const auto drop = DecodeDropSlot(message);
            if (!drop || !scheduler_ || drop->slot >= slotCount_ || drop->slot == slot_)
            {
                Fail("the relay sent a malformed or impossible slot drop");
            }
            else if (!scheduler_->DropSlot(drop->slot, drop->fromTick))
            {
                Fail(std::format("the relay dropped slot {} from tick {}, which this peer cannot apply (it is at tick {})",
                                 drop->slot, drop->fromTick, scheduler_->CurrentTick()));
            }
            else
            {
                Notify(LockstepEvent::Type::SlotDropped, drop->slot, drop->fromTick, std::string(ToString(drop->reason)));
            }
            break;
        }
        case static_cast<std::uint8_t>(RelayMessage::Stop):
            if (const auto stop = DecodeStop(message))
            {
                if (!pendingStop_ && !stop_)
                {
                    pendingStop_ = *stop;
                }
            }
            else
            {
                Fail("the relay sent a malformed stop");
            }
            break;
        case static_cast<std::uint8_t>(RelayMessage::From):
            OnForwardFrom(message, now);
            break;
        default:
            Fail(std::format("the relay sent a message this client does not take (type 0x{:02X})", message[0]));
            break;
        }
    }

    void LockstepRuntime::OnWelcome(const WelcomeMessage &welcome)
    {
        const std::optional<N2Net::LockstepLayoutInfo> info = MakeLockstepLayoutInfo(options_.layout);
        if (welcome.version != kLockstepWireVersion || !info || welcome.layoutHash != options_.layout.Hash() ||
            welcome.payloadBytes != options_.layout.PayloadBytes())
        {
            Fail("the relay's welcome does not match this client's version or input layout");
            return;
        }
        slot_ = welcome.slot;
        slotCount_ = welcome.slotCount;
        hashInterval_ = welcome.hashInterval;
        tickRate_ = welcome.tickRate;
        step_ = 1.0 / static_cast<double>(std::max<std::uint32_t>(tickRate_, 1));
        N2Net::LockstepConfig config;
        config.slotCount = slotCount_;
        config.inputDelay = welcome.inputDelay;
        config.stallTimeoutPolls = 0xFFFFFFFFu;
        config.dropFill = welcome.dropFill == 1 ? N2Net::DropFill::HoldLast : N2Net::DropFill::Neutral;
        config.layout = *info;
        scheduler_ = std::make_unique<N2Net::LockstepScheduler>(config);
        recorder_ = std::make_unique<Replay::LockstepRecorder>(options_.scene, options_.seed,
                                                              static_cast<int>(hashInterval_), static_cast<int>(slotCount_),
                                                              options_.layout);
        responder_ = std::make_unique<N2Net::BisectResponder>(history_, options_.responder, now_);
        state_ = LockstepState::Lobby;
        Notify(LockstepEvent::Type::Welcomed, slot_, 0, std::format("slots={} tickRate={} hashInterval={}", slotCount_,
                                                                   tickRate_, hashInterval_));
    }

    void LockstepRuntime::OnBundle(const std::span<const std::uint8_t> message)
    {
        if (!scheduler_ || (state_ != LockstepState::Running && state_ != LockstepState::Lobby))
        {
            return;
        }
        ++stats_.bundlesReceived;
        const auto bundle = DecodeInputBundleFor(options_.layout, message);
        if (!bundle || bundle->slot >= slotCount_)
        {
            ++stats_.bundlesRejected;
            return;
        }
        if (Ahead(bundle->ackTick, relayAck_))
        {
            relayAck_ = bundle->ackTick;
        }
        if (bundle->slot == slot_)
        {
            return; // the ack of this client's own frames
        }
        std::uint32_t tick = bundle->firstTick;
        for (const N2Net::LockstepInput &input : bundle->frames)
        {
            (void)scheduler_->Submit(bundle->slot, tick, input);
            ++tick;
        }
    }

    void LockstepRuntime::OnForwardFrom(const std::span<const std::uint8_t> message, const std::uint64_t now)
    {
        const auto forward = DecodeForward(RelayMessage::From, message);
        if (!forward)
        {
            Fail("the relay sent a malformed forwarded message");
            return;
        }
        const std::uint8_t kind = N2Net::PeekMessageKind(forward->inner);
        if (kind == N2Net::kBisectRequestKind)
        {
            // Questions are answered from the checkpoint trees, rate limited, whatever state this peer is in
            if (!responder_)
            {
                return;
            }
            std::span<const std::uint8_t> reply;
            if (responder_->Serve(now, forward->inner, reply) == N2Net::ServeResult::Replied)
            {
                ++stats_.bisectRequestsServed;
                SendReliable(EncodeForward(RelayMessage::To, static_cast<std::uint8_t>(forward->slot), reply));
            }
        }
        else if (bisectClient_)
        {
            (void)bisectClient_->OnMessage(now, forward->inner);
        }
    }

    void LockstepRuntime::Pump(const std::uint64_t now)
    {
        now_ = now;
        transport_.Poll([this, now](const N2Net::TransportEvent &event) { HandleEvent(event, now); });
        if (connection_ && !Finished())
        {
            if (connection_->IsFailed())
            {
                Fail("the connection to the relay failed");
            }
        }
        if (connection_)
        {
            (void)connection_->Update(now, [this](const std::span<const std::uint8_t> packet)
                                      { (void)transport_.Send(server_, packet); });
        }
    }

    bool LockstepRuntime::RunOneTick(const N2Net::TickInputs &inputs)
    {
        if (options_.applyTick && !options_.applyTick(inputs, slotCount_))
        {
            Fail(std::format("the simulation failed at tick {}", inputs.tick));
            return false;
        }
        if (options_.ownsTickCounter)
        {
            clock_.BeginTick();
        }
        ++stats_.ticksRun;
        if (recorder_ && !recorderFull_ &&
            !recorder_->RecordInputs(std::span<const N2Net::LockstepInput>(inputs.inputs.data(), slotCount_)))
        {
            recorderFull_ = true;
        }
        const std::uint32_t done = scheduler_->CurrentTick();
        if (options_.buildTree && hashInterval_ != 0 && done % hashInterval_ == 0)
        {
            if (std::optional<N2Net::HashTree> tree = options_.buildTree(done))
            {
                const N2Net::RootMessage root{done, tree->Root()};
                std::array<std::uint8_t, N2Net::kRootMessageSize> bytes{};
                const std::size_t size = N2Net::EncodeRootMessage(root, bytes);
                history_.Push(std::move(*tree));
                if (size != 0)
                {
                    SendReliable(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)));
                    ++stats_.rootsSent;
                }
            }
        }
        return true;
    }

    void LockstepRuntime::RunTicks(const double unscaledDelta)
    {
        clock_.BeginFrame(unscaledDelta);
        const std::uint32_t delay = scheduler_->Config().inputDelay;
        while (state_ == LockstepState::Running && clock_.TickDue(step_))
        {
            // The local slot never runs further ahead than the input delay needs, so a long stall does not queue up
            // inputs that would be sampled long before they are used
            if (static_cast<std::int32_t>(scheduler_->NextNeeded(slot_) - scheduler_->CurrentTick()) <=
                static_cast<std::int32_t>(delay))
            {
                std::vector<float> values = options_.sampleInput
                                                ? options_.sampleInput(scheduler_->NextNeeded(slot_))
                                                : options_.layout.NeutralValues();
                const auto payload = options_.layout.Pack(values);
                if (!payload)
                {
                    Fail("input sampling failed: the sampled input has the wrong number of values");
                    return;
                }
                const auto input = ToLockstepInput(options_.layout, std::span<const std::uint8_t>(*payload));
                if (!input)
                {
                    Fail("input sampling failed: " + input.error());
                    return;
                }
                std::uint32_t assigned = 0;
                (void)scheduler_->SubmitLocal(slot_, *input, assigned);
            }
            N2Net::TickInputs inputs;
            if (scheduler_->TryAdvance(inputs) != N2Net::AdvanceStatus::Runnable)
            {
                ++stats_.stalls;
                clock_.ResetAccumulator();
                break;
            }
            clock_.CompleteTick(step_);
            if (!RunOneTick(inputs))
            {
                return;
            }
        }
        clock_.EndFrame(step_);
    }

    void LockstepRuntime::SendBundle(const std::uint64_t now)
    {
        if (!connection_ || !scheduler_ || state_ != LockstepState::Running || now - lastSendAt_ < options_.sendIntervalMicros)
        {
            return;
        }
        lastSendAt_ = now;
        N2Net::InputBundle bundle = N2Net::BuildBundleFor(*scheduler_, slot_, relayAck_);
        const std::size_t payload = options_.layout.PayloadBytes();
        std::array<std::uint8_t, N2Net::kMaxPacketSize> buffer{};
        const std::size_t room = std::min(connection_->MaxUnreliableMessageSize(), buffer.size());
        const std::size_t overhead = N2Net::InputBundleSize(0, payload);
        if (room < overhead || payload == 0)
        {
            return;
        }
        bundle.frames.resize(std::min(bundle.frames.size(), (room - overhead) / payload));
        if (bundle.frames.empty() && now - lastAckAt_ < kAckIntervalMicros)
        {
            return;
        }
        const std::size_t size = N2Net::EncodeInputBundle(bundle, buffer);
        if (size != 0 && connection_->SendUnreliable(std::span<const std::uint8_t>(buffer.data(), size)) == N2Net::SendResult::Ok)
        {
            ++stats_.bundlesSent;
            lastAckAt_ = now;
        }
    }

    void LockstepRuntime::WriteReport(LockstepStopInfo &info, const N2Net::HashTree *local)
    {
        if (options_.reportPath.empty() || !recorder_)
        {
            return;
        }
        Replay::DesyncReport report;
        report.peerName = options_.peerName;
        report.tick = info.tick;
        report.localRoot = local != nullptr ? local->Root() : (slot_ == info.slotA ? info.rootA : info.rootB);
        report.remoteRoot = slot_ == info.slotA ? info.rootB : info.rootA;
        report.buildFingerprintJson = Replay::BuildFingerprintJson();
        report.determinismFingerprintJson = options_.determinismFingerprintJson;
        report.inputLog = recorder_->Log();
        report.trees = &history_.Trees();
        report.divergence = info.divergence;
        info.reportWritten = Replay::WriteDesyncReport(report, options_.reportPath, info.reportError);
        if (info.reportWritten)
        {
            std::string text = options_.reportPath;
            if (options_.describeField && info.divergence.kind != N2Net::Divergence::Kind::None &&
                info.divergence.kind != N2Net::Divergence::Kind::Malformed)
            {
                const std::string name = options_.describeField(info.divergence.objectId, info.divergence.fieldId);
                if (!name.empty())
                {
                    text += " (" + name + ")";
                }
            }
            Notify(LockstepEvent::Type::ReportWritten, slot_, info.tick, std::move(text));
        }
    }

    void LockstepRuntime::BeginStop(const StopMessage &stopMessage)
    {
        LockstepStopInfo info;
        info.reason = stopMessage.reason;
        info.tick = stopMessage.tick;
        info.slotA = stopMessage.slotA;
        info.slotB = stopMessage.slotB;
        info.rootA = stopMessage.rootA;
        info.rootB = stopMessage.rootB;
        stop_ = info;
        Notify(LockstepEvent::Type::Stopped, info.slotA, info.tick, std::string(ToString(info.reason)));

        if (info.reason != StopReason::Desync)
        {
            FinishStop();
            return;
        }
        N2Net::HashTree *const local = history_.Find(info.tick);
        const bool participant = slot_ == info.slotA || slot_ == info.slotB;
#if !defined(__EMSCRIPTEN__)
        if (participant && local != nullptr && connection_)
        {
            const std::uint32_t other = slot_ == info.slotA ? info.slotB : info.slotA;
            bisectClient_ = std::make_unique<N2Net::BisectClient>(
                [this, other](const std::span<const std::uint8_t> message)
                {
                    const std::vector<std::uint8_t> envelope =
                        EncodeForward(RelayMessage::To, static_cast<std::uint8_t>(other), message);
                    return !envelope.empty() && connection_ && connection_->SendReliable(envelope) == N2Net::SendResult::Ok;
                },
                options_.bisect);
            bisection_ = std::make_unique<Coroutine>(
                [this, local](Coroutine &co)
                {
                    DriverImpl driver(*this, co);
                    N2Net::NetworkedHashTreePeer peer(*bisectClient_, driver, stop_->tick);
                    stop_->divergence = N2Net::Bisect(*local, peer);
                    stop_->bisectFailure = peer.Failure();
                });
            stopPhase_ = StopPhase::Bisecting;
            return;
        }
#endif
        (void)participant;
        WriteReport(*stop_, local);
        // A participant that could not bisect has nothing more to serve
        FinishStop();
    }

    void LockstepRuntime::StepStop(const std::uint64_t now)
    {
#if !defined(__EMSCRIPTEN__)
        if (stopPhase_ == StopPhase::Bisecting)
        {
            bisection_->Resume();
            if (bisection_->done)
            {
                bisection_.reset();
                bisectClient_.reset();
                WriteReport(*stop_, history_.Find(stop_->tick));
                // Keep answering the other peer's questions until it is done (it does not tell us), then leave
                stopPhase_ = StopPhase::Lingering;
                lingerUntil_ = now + options_.desyncLingerMicros;
            }
        }
        else if (stopPhase_ == StopPhase::Lingering && now >= lingerUntil_)
        {
            FinishStop();
        }
#else
        (void)now;
#endif
    }

    void LockstepRuntime::FinishStop()
    {
        stopPhase_ = StopPhase::None;
        if (server_ != N2Net::kInvalidPeer)
        {
            // Let the last packets out, then leave
            if (connection_)
            {
                (void)connection_->Update(now_, [this](const std::span<const std::uint8_t> packet)
                                          { (void)transport_.Send(server_, packet); });
            }
            transport_.Disconnect(server_);
        }
        if (!Finished())
        {
            state_ = LockstepState::Stopped;
        }
    }

    void LockstepRuntime::Update(const std::uint64_t nowMicros, const double unscaledDeltaSeconds)
    {
        if (!haveStartedAt_)
        {
            haveStartedAt_ = true;
            startedAt_ = nowMicros;
        }
        Pump(nowMicros);
        if (pendingStop_ && !Finished())
        {
            const StopMessage stop = *pendingStop_;
            pendingStop_.reset();
            BeginStop(stop);
        }
        if (stopPhase_ != StopPhase::None && !Finished())
        {
            StepStop(nowMicros);
            Pump(nowMicros);
            return;
        }
        if ((state_ == LockstepState::Connecting || state_ == LockstepState::Joining) &&
            nowMicros - startedAt_ >= options_.joinTimeoutMicros)
        {
            Fail("the relay did not answer the join in time");
        }
        if (state_ == LockstepState::Running && scheduler_ && !stop_)
        {
            RunTicks(unscaledDeltaSeconds);
            SendBundle(nowMicros);
            Pump(nowMicros);
        }
    }
}
