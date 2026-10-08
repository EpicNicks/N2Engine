#include "editor-server/EditHistory.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <unordered_set>

namespace N2Engine::Editor
{
    namespace
    {
        /// What a step costs besides what its ops report: the closures, the label
        constexpr size_t BytesPerOp = 128;
    }

    void EditEffect::Merge(EditEffect other)
    {
        full = full || other.full;
        inexact = inexact || other.inexact;
        std::unordered_set<std::string> known(entityIds.begin(), entityIds.end());
        for (std::string &id : other.entityIds)
        {
            if (known.insert(id).second)
            {
                entityIds.push_back(std::move(id));
            }
        }
    }

    size_t EditHistory::BytesOf(const Step &step)
    {
        size_t bytes = step.label.size();
        for (const EditOp &op : step.ops)
        {
            bytes += BytesPerOp + op.undoBytes + op.redoBytes + op.coalesceKey.size();
        }
        return bytes;
    }

    void EditHistory::MergeOps(EditOp &into, EditOp later)
    {
        into.redo = std::move(later.redo);
        into.redoBytes = later.redoBytes;
    }

    EditHistory::Recorded EditHistory::Record(std::string label, EditOp op, const Clock::time_point now)
    {
        if (!op.undo || !op.redo)
        {
            return Recorded::Ignored;
        }

        if (_groupDepth > 0)
        {
            // Inside a group the ops of one thing (the same object's transform, every frame of a drag) are one op
            // however many arrive, so a long drag holds one before and one after
            for (size_t i = _groupOps.size(); i-- > 0;)
            {
                EditOp &earlier = _groupOps[i];
                if (!op.coalesceKey.empty() && earlier.coalesceKey == op.coalesceKey)
                {
                    _groupBytes = _groupBytes - earlier.redoBytes + op.redoBytes;
                    MergeOps(earlier, std::move(op));
                    return Recorded::InGroup;
                }
                if (!(earlier.commutes && op.commutes))
                {
                    break;
                }
            }
            _groupBytes += op.undoBytes + op.redoBytes + BytesPerOp;
            _groupOps.push_back(std::move(op));
            return Recorded::InGroup;
        }

        if (!op.coalesceKey.empty() && !_steps.empty() && _cursor == _steps.size())
        {
            Step &top = _steps.back();
            if (top.coalescible && top.ops.size() == 1 && top.ops.front().coalesceKey == op.coalesceKey &&
                now >= top.lastEdit && now - top.lastEdit <= _coalesceWindow)
            {
                // The state the step led to (as saved, perhaps) is replaced by a later one
                if (_markerValid && _markerSerial == top.serial)
                {
                    _markerValid = false;
                }
                _bytes -= top.bytes;
                MergeOps(top.ops.front(), std::move(op));
                top.lastEdit = now;
                top.serial = ++_nextSerial;
                top.bytes = BytesOf(top);
                _bytes += top.bytes;
                Enforce();
                return Recorded::Coalesced;
            }
        }

        Step step;
        step.label = std::move(label);
        step.coalescible = !op.coalesceKey.empty();
        step.ops.push_back(std::move(op));
        step.serial = ++_nextSerial;
        step.lastEdit = now;
        Push(std::move(step));
        return Recorded::NewStep;
    }

    void EditHistory::Push(Step step)
    {
        // What was undone can't be redone once something else was done
        for (size_t i = _cursor; i < _steps.size(); ++i)
        {
            if (_markerValid && _markerSerial == _steps[i].serial)
            {
                _markerValid = false;
            }
            _bytes -= _steps[i].bytes;
        }
        _steps.erase(_steps.begin() + static_cast<std::ptrdiff_t>(_cursor), _steps.end());

        step.bytes = BytesOf(step);
        _bytes += step.bytes;
        _steps.push_back(std::move(step));
        _cursor = _steps.size();
        Enforce();
    }

    void EditHistory::EvictOldest()
    {
        if (_cursor > 0)
        {
            // The state before the oldest step goes, and the one after it becomes the first
            if (_markerValid && _markerSerial == _baseSerial)
            {
                _markerValid = false;
            }
            _baseSerial = _steps.front().serial;
            _bytes -= _steps.front().bytes;
            _steps.erase(_steps.begin());
            --_cursor;
        }
        else
        {
            // Nothing is done: the limit can only be met by dropping what was undone, newest first
            if (_markerValid && _markerSerial == _steps.back().serial)
            {
                _markerValid = false;
            }
            _bytes -= _steps.back().bytes;
            _steps.pop_back();
        }
    }

    void EditHistory::Enforce()
    {
        while (_steps.size() > _maxSteps || (_bytes > _maxBytes && _steps.size() > 1))
        {
            EvictOldest();
        }
    }

    void EditHistory::SetLimits(const size_t maxSteps, const size_t maxBytes)
    {
        _maxSteps = std::max<size_t>(maxSteps, 1);
        _maxBytes = std::max<size_t>(maxBytes, 1);
        Enforce();
    }

    bool EditHistory::BeginGroup(std::string label)
    {
        if (_groupDepth >= MaxGroupDepth)
        {
            return false;
        }
        if (_groupDepth == 0)
        {
            _groupLabel = std::move(label);
            _groupOps.clear();
            _groupBytes = 0;
        }
        ++_groupDepth;
        return true;
    }

    EditHistory::GroupEnd EditHistory::FinishGroup()
    {
        _groupDepth = 0;
        std::vector<EditOp> ops = std::move(_groupOps);
        _groupOps.clear();
        _groupBytes = 0;
        if (ops.empty())
        {
            return GroupEnd::Empty;
        }

        Step step;
        step.label = std::move(_groupLabel);
        step.ops = std::move(ops);
        step.serial = ++_nextSerial;
        step.lastEdit = Clock::now();
        step.coalescible = false;
        Push(std::move(step));
        return GroupEnd::Committed;
    }

    EditHistory::GroupEnd EditHistory::EndGroup()
    {
        if (_groupDepth == 0)
        {
            return GroupEnd::NotOpen;
        }
        if (_groupDepth > 1)
        {
            --_groupDepth;
            return GroupEnd::StillOpen;
        }
        return FinishGroup();
    }

    EditHistory::GroupEnd EditHistory::CloseGroups()
    {
        if (_groupDepth == 0)
        {
            return GroupEnd::NotOpen;
        }
        return FinishGroup();
    }

    std::expected<EditHistory::Applied, std::string> EditHistory::Run(const bool undo)
    {
        if (_groupDepth > 0)
        {
            return std::unexpected(std::format("An edit group is open: end it before {}", undo ? "undoing" : "redoing"));
        }
        if (undo ? _cursor == 0 : _cursor == _steps.size())
        {
            return std::unexpected(std::string(undo ? "Nothing to undo" : "Nothing to redo"));
        }

        // A copy of what is needed: the ops may not touch the history, but the step is only moved over once done
        const size_t index = undo ? _cursor - 1 : _cursor;
        const std::string label = _steps[index].label;
        const size_t count = _steps[index].ops.size();

        EditEffect effect;
        for (size_t n = 0; n < count; ++n)
        {
            // Undo runs the ops last to first, as the edits were made
            const EditOp &op = _steps[index].ops[undo ? count - 1 - n : n];
            std::string failure;
            try
            {
                const EditOutcome outcome = undo ? op.undo() : op.redo();
                if (outcome)
                {
                    effect.Merge(*outcome);
                    continue;
                }
                failure = outcome.error();
            }
            catch (const std::exception &error)
            {
                failure = error.what();
            }
            catch (...)
            {
                failure = "unknown exception";
            }

            // Some of the step may be done: the scene is in a state no step describes, so nothing built on it is safe
            Clear();
            return std::unexpected(std::format("Couldn't {} '{}': {} (the edit history was cleared)",
                                               undo ? "undo" : "redo", label, failure));
        }

        _cursor = undo ? _cursor - 1 : _cursor + 1;
        // A later edit is a step of its own, not part of one that was undone and redone since
        if (_cursor > 0)
        {
            _steps[_cursor - 1].coalescible = false;
        }
        return Applied{label, std::move(effect)};
    }

    std::expected<EditHistory::Applied, std::string> EditHistory::Undo()
    {
        return Run(true);
    }

    std::expected<EditHistory::Applied, std::string> EditHistory::Redo()
    {
        return Run(false);
    }

    std::string EditHistory::UndoLabel() const
    {
        return CanUndo() ? _steps[_cursor - 1].label : std::string{};
    }

    std::string EditHistory::RedoLabel() const
    {
        return CanRedo() ? _steps[_cursor].label : std::string{};
    }

    std::vector<EditHistory::Entry> EditHistory::Entries() const
    {
        std::vector<Entry> entries;
        entries.reserve(_steps.size());
        for (const Step &step : _steps)
        {
            entries.push_back({step.label, step.bytes});
        }
        return entries;
    }

    void EditHistory::Clear()
    {
        _steps.clear();
        _cursor = 0;
        _bytes = 0;
        _groupDepth = 0;
        _groupLabel.clear();
        _groupOps.clear();
        _groupBytes = 0;
        // A state of its own: nothing earlier names it
        _baseSerial = ++_nextSerial;
        _markerValid = false;
    }

    void EditHistory::MarkSaved()
    {
        if (_groupDepth > 0)
        {
            // The open group's step will be a state after the save: no state of the history is the saved one
            _markerValid = false;
            return;
        }
        _markerSerial = StateSerial();
        _markerValid = true;
        // An edit after a save is a new step, so undoing it returns to what was saved
        if (_cursor > 0)
        {
            _steps[_cursor - 1].coalescible = false;
        }
    }

    void EditHistory::ForgetSaved()
    {
        _markerValid = false;
    }
}
