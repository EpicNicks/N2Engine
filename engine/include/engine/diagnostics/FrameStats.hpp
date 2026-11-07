#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace N2Engine::Diagnostics
{
    /// Where one frame's time went (EN5, the editor's Profiler panel). Milliseconds of wall time on the steady clock.
    struct FrameRecord
    {
        /// The frame's number: the play host's game frame (EditorServer: its frame counter, frames Step ran included), or
        /// the count of frames FrameStats has begun (a host that names none)
        std::uint32_t frame = 0;
        /// Wall time from the previous frame's start to this one's (0 for the first frame after the timers were turned on,
        /// or after Application::ResetFrameClock): the frame rate
        float intervalMs = 0.0f;
        /// The whole Application::Tick
        float tickMs = 0.0f;
        /// The fixed-step loop (or a network session's fixed-tick driver), and how many fixed ticks ran in it
        float fixedMs = 0.0f;
        std::uint32_t fixedTicks = 0;
        /// Inside those ticks: the physics backend's step, its transform sync and its collision callbacks (the rest of
        /// fixedMs is FixedUpdate, the attach queue and the loop itself)
        float physicsMs = 0.0f;
        /// The pointer dispatch, Update, coroutines and LateUpdate
        float updateMs = 0.0f;
        /// The audio system's stream and update
        float audioMs = 0.0f;
        /// Drawing, when the Tick draws (a game; the editor's play host never does: it draws on request)
        float renderMs = 0.0f;
        /// Play host: the commands handled after this frame, before the next (their handling time, not the waiting)
        float commandsMs = 0.0f;
        /// Play host: of commandsMs, drawing the game for a RenderFrame, and reading the frame back
        float renderRequestMs = 0.0f;
        float readbackMs = 0.0f;
    };

    /// The parts of a frame FrameStats times (FrameRecord's fields of the same names)
    enum class FrameSection : std::uint8_t
    {
        Fixed,
        Physics,
        Update,
        Audio,
        Render,
        Commands,
        RenderRequest,
        Readback,
    };

    /// avg, 95th percentile and max of one field over the frames held
    struct FrameTiming
    {
        float avg = 0.0f;
        float p95 = 0.0f;
        float max = 0.0f;
    };

    struct FrameSummary
    {
        /// How many frames the summary is over (the frames held, at most FrameStats::HistoryFrames)
        std::size_t windowFrames = 0;
        /// 1000 over the average intervalMs of the frames that have one (0 when none has)
        float fps = 0.0f;
        FrameTiming tickMs;
    };

    /**
     * Per-frame timers for Application::Tick and the editor's play host, and a ring of the last HistoryFrames frames.
     *
     * Off by default: a section then costs one branch (ScopedFrameSection checks IsEnabled once) and nothing is recorded.
     * The editor host turns it on at the first GetEngineStats and off again when nobody has asked for a while. The timers
     * only read the steady clock: nothing they record reaches the simulation, so turning them on changes no game state.
     * The ring is a fixed array (no allocation per frame).
     *
     * A frame is open from BeginFrame to EndFrame (Tick's span): sections add to it. EndFrame stores it as the newest
     * record, which stays open for the time a host spends after the frame (AddToNewestFrame: the play host's commands)
     * until the next BeginFrame or CloseNewestFrame.
     */
    class FrameStats
    {
    public:
        using Clock = std::chrono::steady_clock;
        /// The frames held: 4 s at 60 frames per second
        static constexpr std::size_t HistoryFrames = 240;

        /// Turning it on (from off) starts clean: no frames held, no open frame, no interval. Turning it off leaves the
        /// frames held as they are and stops recording.
        void SetEnabled(bool enabled);
        [[nodiscard]] bool IsEnabled() const { return _enabled; }

        /// The number the next BeginFrame gives its frame (a host that counts frames itself; else the frame after the last)
        void SetNextFrameNumber(std::uint32_t frame) { _nextFrame = frame; }

        /// Starts a frame (Application::Tick, first thing). Does nothing while off. A frame still open (a Tick that
        /// threw) is dropped.
        void BeginFrame(Clock::time_point now = Clock::now());
        /// Ends the open frame (its tickMs is the time since BeginFrame) and stores it as the newest record
        void EndFrame(Clock::time_point now = Clock::now());
        [[nodiscard]] bool IsFrameOpen() const { return _frameOpen; }

        /// Adds `milliseconds` to `section` of the open frame (nothing when no frame is open)
        void Add(FrameSection section, double milliseconds);
        /// One fixed tick ran in the open frame
        void NoteFixedTick();
        /// Adds to the newest record while it is still open for what follows its frame (see the class comment)
        void AddToNewestFrame(FrameSection section, double milliseconds);
        /// The newest record takes nothing more (the play host does this while paused, so a paused game's commands are not
        /// piled on its last frame)
        void CloseNewestFrame() { _newestOpen = false; }
        /// The next frame has no interval (Application::ResetFrameClock: a pause is not a frame)
        void ResetInterval() { _hasLastStart = false; }

        /// The frame number of the newest record (empty when none is held)
        [[nodiscard]] std::optional<std::uint32_t> NewestFrame() const;
        /// The records after frame `sinceFrame`, oldest first: every record whose frame is greater. 0 gives every record
        /// held; so does a sinceFrame past the newest record (the frames started again: a new play session).
        [[nodiscard]] std::vector<FrameRecord> FramesAfter(std::uint32_t sinceFrame) const;
        [[nodiscard]] std::size_t Size() const { return _count; }
        [[nodiscard]] FrameSummary Summarize() const;

    private:
        [[nodiscard]] const FrameRecord &At(std::size_t index) const; // 0 is the oldest held
        static void AddTo(FrameRecord &record, FrameSection section, float milliseconds);

        std::array<FrameRecord, HistoryFrames> _ring{};
        std::size_t _head = 0; // where the next record goes
        std::size_t _count = 0;
        bool _enabled = false;
        bool _frameOpen = false;
        bool _newestOpen = false;
        FrameRecord _open{};
        Clock::time_point _openStart{};
        Clock::time_point _lastStart{};
        bool _hasLastStart = false;
        std::optional<std::uint32_t> _nextFrame;
        std::uint32_t _lastFrame = 0;
    };

    /// Times one section of the open frame: from construction to destruction. Reads the clock only while the stats are on.
    class ScopedFrameSection
    {
    public:
        ScopedFrameSection(FrameStats &stats, const FrameSection section)
            : _stats(stats.IsEnabled() ? &stats : nullptr), _section(section)
        {
            if (_stats != nullptr)
            {
                _start = FrameStats::Clock::now();
            }
        }
        ~ScopedFrameSection()
        {
            if (_stats != nullptr)
            {
                _stats->Add(_section, std::chrono::duration<double, std::milli>(FrameStats::Clock::now() - _start).count());
            }
        }
        ScopedFrameSection(const ScopedFrameSection &) = delete;
        ScopedFrameSection &operator=(const ScopedFrameSection &) = delete;

    private:
        FrameStats *_stats;
        FrameSection _section;
        FrameStats::Clock::time_point _start{};
    };
}
