#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace N2Engine::Editor
{
    /// What an undo or a redo touched, in the terms of sceneChanged: the objects whose state moved (their UUID
    /// strings), or full when the loaded scene was rebuilt (every id a client holds must be refetched)
    struct EditEffect
    {
        std::vector<std::string> entityIds;
        bool full = false;
        /// The change couldn't put the state back exactly (an object can't lose the transform it was given), so the
        /// state it leads to is not the one that was saved even if the history says it is
        bool inexact = false;

        /// Adds what `other` touched (an id already listed is not listed twice)
        void Merge(EditEffect other);
    };

    /// The result of running one half of an edit: what it touched, or why it couldn't be done
    using EditOutcome = std::expected<EditEffect, std::string>;

    /**
     * One reversible change to the scene. Both halves address what they change by UUID, looked up when they run
     * (never by pointer), so they stay valid across a snapshot restore that rebuilt every object.
     */
    struct EditOp
    {
        /// Puts back the state from before the change
        std::function<EditOutcome()> undo;
        /// Makes the change again, from the state undo left
        std::function<EditOutcome()> redo;
        /// What the closures hold in memory (JSON, snapshots) on the undo side and on the redo side, for the limit
        size_t undoBytes = 0;
        size_t redoBytes = 0;
        /// Non-empty: this op sets one thing (a field, a transform) that a later op with the same key sets again, so
        /// the later one replaces this one's redo and keeps its undo (typing in a number box is one step)
        std::string coalesceKey;
        /// The op sets a state that doesn't depend on any other op's, so inside a group it may be merged with an
        /// earlier op of the same key past ops of other keys (dragging a selection moves several objects every frame)
        bool commutes = false;
    };

    /**
     * The editor's undo/redo stack. A step is one or more ops (a group: a gizmo drag, a paste of several fields) that
     * undo and redo together. It knows nothing of scenes: an op is a pair of closures (see EditOp), so the engine
     * state it needs lives in the server that records them.
     *
     * Limits: at most MaxSteps steps and MaxBytes of what the ops hold; recording past either drops the oldest steps
     * (the newest step is always kept, even when it alone is over MaxBytes). Recording after an undo drops the steps
     * that were undone (the redo stack), as every editor does.
     *
     * Saved state: MarkSaved remembers the state the history is at (the scene as saved); IsAtSavedState is true
     * whenever the history is at that state again, which is how undoing back to the saved scene clears "unsaved
     * changes". The state is gone, and IsAtSavedState false until the next MarkSaved, once an edit merged into the
     * step that produced it, a new edit discarded the redo steps leading to it, or eviction dropped the steps that
     * led to it.
     *
     * Not thread-safe: the editor server uses it on the main thread only.
     */
    class EditHistory
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr size_t DefaultMaxSteps = 200;
        static constexpr size_t DefaultMaxBytes = 128ull * 1024 * 1024;
        static constexpr std::chrono::milliseconds DefaultCoalesceWindow{500};
        /// How deep BeginGroup nests
        static constexpr size_t MaxGroupDepth = 16;

        /// What Undo and Redo did
        struct Applied
        {
            std::string label;
            EditEffect effect;
        };

        /// One step, for an Edit menu
        struct Entry
        {
            std::string label;
            size_t bytes = 0;
        };

        enum class Recorded
        {
            /// The op had no undo or redo: nothing was recorded
            Ignored,
            /// A step of its own
            NewStep,
            /// Merged into the step before it (the same key within the coalesce window)
            Coalesced,
            /// Added to the open group, which becomes a step when it ends
            InGroup,
        };

        enum class GroupEnd
        {
            /// No group was open
            NotOpen,
            /// An inner group ended; the outer one is still open
            StillOpen,
            /// The outermost group ended with no op in it: no step
            Empty,
            /// The outermost group ended and its ops became one step
            Committed,
        };

        /// Records an op as a step labelled `label`, or into the open group (whose label it keeps), or merged into
        /// the step before it. `now` is the time of the edit (tests pass their own).
        Recorded Record(std::string label, EditOp op, Clock::time_point now = Clock::now());

        /// Starts a group (the ops recorded until the matching EndGroup are one step, labelled `label`); a group in
        /// a group is part of the outer one and its label is ignored. False, opening nothing, past MaxGroupDepth.
        bool BeginGroup(std::string label);
        /// Ends the innermost group; the outermost one's end makes its step
        GroupEnd EndGroup();
        /// Ends every open group (a client that went away without ending its group). NotOpen when none was open.
        GroupEnd CloseGroups();
        [[nodiscard]] bool InGroup() const { return _groupDepth > 0; }
        /// What the open group's ops hold (0 without one); it counts against no limit until the group ends, so a caller
        /// that records big ops into a group (snapshots) checks it against GetMaxBytes itself
        [[nodiscard]] size_t GroupBytes() const { return _groupBytes; }
        [[nodiscard]] size_t GroupDepth() const { return _groupDepth; }

        /// Undoes the latest step that is done. An error (with nothing changed) when there is none or a group is
        /// open. When an op of the step fails, the history is cleared, since the scene may now be in a state no
        /// step describes, and the error says so.
        std::expected<Applied, std::string> Undo();
        /// Redoes the step undone last; as Undo
        std::expected<Applied, std::string> Redo();

        [[nodiscard]] bool CanUndo() const { return _groupDepth == 0 && _cursor > 0; }
        [[nodiscard]] bool CanRedo() const { return _groupDepth == 0 && _cursor < _steps.size(); }
        /// The label of the step Undo would undo, or empty
        [[nodiscard]] std::string UndoLabel() const;
        /// The label of the step Redo would redo, or empty
        [[nodiscard]] std::string RedoLabel() const;

        /// Every step, oldest first; the first Cursor() of them are done (undoable), the rest undone (redoable)
        [[nodiscard]] std::vector<Entry> Entries() const;
        [[nodiscard]] size_t Cursor() const { return _cursor; }
        [[nodiscard]] size_t StepCount() const { return _steps.size(); }
        /// What the steps hold, as the ops reported it
        [[nodiscard]] size_t TotalBytes() const { return _bytes; }

        /// Forgets everything, an open group included. Nothing is the saved state afterwards, until MarkSaved.
        void Clear();

        /// The history is at the state that was saved (the scene as it is on disk): IsAtSavedState now is true. While
        /// a group is open the state it will lead to isn't known yet, so a save then forgets the saved state instead.
        void MarkSaved();
        /// The saved state is no longer reachable: IsAtSavedState is false until MarkSaved
        void ForgetSaved();
        [[nodiscard]] bool IsAtSavedState() const { return _markerValid && _markerSerial == StateSerial(); }

        /// Changes the limits (at least one step and one byte each), dropping the oldest steps that are over them
        void SetLimits(size_t maxSteps, size_t maxBytes);
        [[nodiscard]] size_t GetMaxSteps() const { return _maxSteps; }
        [[nodiscard]] size_t GetMaxBytes() const { return _maxBytes; }
        void SetCoalesceWindow(std::chrono::milliseconds window) { _coalesceWindow = window; }
        [[nodiscard]] std::chrono::milliseconds GetCoalesceWindow() const { return _coalesceWindow; }

    private:
        struct Step
        {
            std::string label;
            std::vector<EditOp> ops;
            /// Names the state after this step; changes when the step does (an edit merged into it)
            uint64_t serial = 0;
            Clock::time_point lastEdit{};
            /// A later single edit may merge into it (not a group's step, and not after a save)
            bool coalescible = false;
            size_t bytes = 0;
        };

        static size_t BytesOf(const Step &step);
        /// Replaces `into`'s redo by the one of `later` (both set the same thing); `into` keeps its undo
        static void MergeOps(EditOp &into, EditOp later);
        [[nodiscard]] uint64_t StateSerial() const { return _cursor == 0 ? _baseSerial : _steps[_cursor - 1].serial; }

        /// Appends a step after the done ones (dropping the undone ones) and enforces the limits
        void Push(Step step);
        void Enforce();
        void EvictOldest();
        GroupEnd FinishGroup();
        std::expected<Applied, std::string> Run(bool undo);

        std::vector<Step> _steps;
        /// Steps [0, _cursor) are done
        size_t _cursor = 0;
        size_t _bytes = 0;
        size_t _maxSteps = DefaultMaxSteps;
        size_t _maxBytes = DefaultMaxBytes;
        std::chrono::milliseconds _coalesceWindow = DefaultCoalesceWindow;

        uint64_t _nextSerial = 0;
        /// The state with no step done: after the oldest step the limits dropped, or the first state
        uint64_t _baseSerial = 0;
        uint64_t _markerSerial = 0;
        bool _markerValid = false;

        size_t _groupDepth = 0;
        std::string _groupLabel;
        std::vector<EditOp> _groupOps;
        size_t _groupBytes = 0;
    };
}
