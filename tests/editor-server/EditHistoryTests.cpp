#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <editor-server/EditHistory.hpp>

using namespace N2Engine::Editor;
using namespace std::chrono_literals;

// EditHistory on its own (#78, E6): steps, groups, coalescing, limits, the saved state. A "document" of one int stands
// in for the scene; the ops address it the way the server's address the scene, by what they set (state-based).
namespace
{
    struct Doc
    {
        int value = 0;
        int other = 0;
        std::vector<std::string> log;
    };

    EditEffect Touched(const std::string &id)
    {
        EditEffect effect;
        effect.entityIds.push_back(id);
        return effect;
    }

    /// An op that sets doc.value from `before` to `after`
    EditOp SetValue(Doc &doc, const int before, const int after, std::string key = {}, const size_t bytes = 0)
    {
        EditOp op;
        op.undo = [&doc, before]() -> EditOutcome
        {
            doc.value = before;
            doc.log.push_back("undo value " + std::to_string(before));
            return Touched("value");
        };
        op.redo = [&doc, after]() -> EditOutcome
        {
            doc.value = after;
            doc.log.push_back("redo value " + std::to_string(after));
            return Touched("value");
        };
        op.undoBytes = bytes;
        op.redoBytes = bytes;
        op.coalesceKey = std::move(key);
        return op;
    }

    /// An op that sets doc.other
    EditOp SetOther(Doc &doc, const int before, const int after, std::string key = {})
    {
        EditOp op;
        op.undo = [&doc, before]() -> EditOutcome
        {
            doc.other = before;
            return Touched("other");
        };
        op.redo = [&doc, after]() -> EditOutcome
        {
            doc.other = after;
            return Touched("other");
        };
        op.coalesceKey = std::move(key);
        return op;
    }

    /// Does the edit and records it
    EditHistory::Recorded Edit(EditHistory &history, Doc &doc, const std::string &label, const int after,
                               const std::string &key = {}, const EditHistory::Clock::time_point now = EditHistory::Clock::now(),
                               const size_t bytes = 0)
    {
        const int before = doc.value;
        doc.value = after;
        return history.Record(label, SetValue(doc, before, after, key, bytes), now);
    }

    const EditHistory::Clock::time_point T0 = EditHistory::Clock::now();
}

TEST(EditHistoryTest, StartsEmpty)
{
    EditHistory history;
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.StepCount(), 0u);
    EXPECT_EQ(history.UndoLabel(), "");
    EXPECT_EQ(history.RedoLabel(), "");
    EXPECT_FALSE(history.IsAtSavedState());

    const auto undone = history.Undo();
    ASSERT_FALSE(undone.has_value());
    EXPECT_NE(undone.error().find("Nothing to undo"), std::string::npos);
    const auto redone = history.Redo();
    ASSERT_FALSE(redone.has_value());
    EXPECT_NE(redone.error().find("Nothing to redo"), std::string::npos);
}

TEST(EditHistoryTest, DefaultLimitsAreTheProposals)
{
    EditHistory history;
    EXPECT_EQ(history.GetMaxSteps(), 200u);
    EXPECT_EQ(history.GetMaxBytes(), 128u * 1024u * 1024u);
    EXPECT_EQ(history.GetCoalesceWindow(), 500ms);
}

TEST(EditHistoryTest, UndoAndRedoWalkTheSteps)
{
    EditHistory history;
    Doc doc;
    EXPECT_EQ(Edit(history, doc, "one", 1), EditHistory::Recorded::NewStep);
    EXPECT_EQ(Edit(history, doc, "two", 2), EditHistory::Recorded::NewStep);
    EXPECT_EQ(Edit(history, doc, "three", 3), EditHistory::Recorded::NewStep);

    EXPECT_TRUE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.UndoLabel(), "three");
    EXPECT_EQ(history.Cursor(), 3u);

    auto undone = history.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->label, "three");
    EXPECT_EQ(doc.value, 2);
    ASSERT_EQ(undone->effect.entityIds.size(), 1u);
    EXPECT_EQ(undone->effect.entityIds[0], "value");
    EXPECT_FALSE(undone->effect.full);
    EXPECT_EQ(history.RedoLabel(), "three");
    EXPECT_EQ(history.UndoLabel(), "two");

    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 0);
    EXPECT_FALSE(history.CanUndo());
    EXPECT_TRUE(history.CanRedo());
    EXPECT_EQ(history.Cursor(), 0u);

    auto redone = history.Redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_EQ(redone->label, "one");
    EXPECT_EQ(doc.value, 1);
    ASSERT_TRUE(history.Redo().has_value());
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_EQ(doc.value, 3);
    EXPECT_FALSE(history.CanRedo());
    EXPECT_FALSE(history.Redo().has_value());
}

TEST(EditHistoryTest, RecordingAfterAnUndoDropsTheRedoSteps)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);
    Edit(history, doc, "two", 2);
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.CanRedo());

    Edit(history, doc, "branch", 10);
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.StepCount(), 2u);
    EXPECT_EQ(history.UndoLabel(), "branch");
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 1);
}

TEST(EditHistoryTest, AnOpWithoutBothHalvesIsIgnored)
{
    EditHistory history;
    EditOp half;
    half.undo = []() -> EditOutcome { return EditEffect{}; };
    EXPECT_EQ(history.Record("half", half), EditHistory::Recorded::Ignored);
    EXPECT_EQ(history.StepCount(), 0u);
}

TEST(EditHistoryTest, EntriesListEveryStepOldestFirstWithTheCursor)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1, {}, T0, 1000);
    Edit(history, doc, "two", 2);
    ASSERT_TRUE(history.Undo().has_value());

    const auto entries = history.Entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].label, "one");
    EXPECT_EQ(entries[1].label, "two");
    EXPECT_GT(entries[0].bytes, 2000u);
    EXPECT_EQ(history.Cursor(), 1u);
    EXPECT_EQ(history.TotalBytes(), entries[0].bytes + entries[1].bytes);
}

// ==================== Effects ====================

TEST(EditHistoryTest, EffectMergeListsAnIdOnceAndKeepsFull)
{
    EditEffect first;
    first.entityIds = {"a", "b"};
    EditEffect second;
    second.entityIds = {"b", "c"};
    second.full = true;
    first.Merge(std::move(second));
    ASSERT_EQ(first.entityIds.size(), 3u);
    EXPECT_EQ(first.entityIds[0], "a");
    EXPECT_EQ(first.entityIds[1], "b");
    EXPECT_EQ(first.entityIds[2], "c");
    EXPECT_TRUE(first.full);
}

// ==================== Coalescing ====================

TEST(EditHistoryTest, SameKeyWithinTheWindowIsOneStep)
{
    EditHistory history;
    Doc doc;
    EXPECT_EQ(Edit(history, doc, "Set count", 1, "count", T0), EditHistory::Recorded::NewStep);
    EXPECT_EQ(Edit(history, doc, "Set count", 2, "count", T0 + 100ms), EditHistory::Recorded::Coalesced);
    EXPECT_EQ(Edit(history, doc, "Set count", 3, "count", T0 + 250ms), EditHistory::Recorded::Coalesced);
    EXPECT_EQ(history.StepCount(), 1u);

    // One undo goes back to before the first, one redo to after the last
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 0);
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_EQ(doc.value, 3);
}

TEST(EditHistoryTest, TheWindowRunsFromTheLatestEdit)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    // 400 ms after each: all one step, though the last is 800 ms after the first
    EXPECT_EQ(Edit(history, doc, "Set", 2, "k", T0 + 400ms), EditHistory::Recorded::Coalesced);
    EXPECT_EQ(Edit(history, doc, "Set", 3, "k", T0 + 800ms), EditHistory::Recorded::Coalesced);
    EXPECT_EQ(history.StepCount(), 1u);
}

TEST(EditHistoryTest, AfterTheWindowIsANewStep)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    EXPECT_EQ(Edit(history, doc, "Set", 2, "k", T0 + 501ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(history.StepCount(), 2u);
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 1);
}

TEST(EditHistoryTest, ADifferentKeyOrNoKeyIsANewStep)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    EXPECT_EQ(Edit(history, doc, "Set", 2, "other", T0 + 10ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(Edit(history, doc, "Set", 3, "", T0 + 20ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(Edit(history, doc, "Set", 4, "", T0 + 30ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(history.StepCount(), 4u);
}

TEST(EditHistoryTest, AnUndoneStepIsNotCoalescedInto)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(Edit(history, doc, "Set", 2, "k", T0 + 10ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(history.StepCount(), 1u);
}

TEST(EditHistoryTest, ASaveEndsCoalescing)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    history.MarkSaved();
    EXPECT_EQ(Edit(history, doc, "Set", 2, "k", T0 + 10ms), EditHistory::Recorded::NewStep);
    // The saved state is one undo away, and reachable
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.IsAtSavedState());
    EXPECT_EQ(doc.value, 1);
}

// ==================== Groups ====================

TEST(EditHistoryTest, AGroupIsOneStepUndoneInReverse)
{
    EditHistory history;
    Doc doc;
    ASSERT_TRUE(history.BeginGroup("Paste"));
    EXPECT_TRUE(history.InGroup());

    doc.value = 1;
    EXPECT_EQ(history.Record("ignored label", SetValue(doc, 0, 1)), EditHistory::Recorded::InGroup);
    doc.other = 5;
    EXPECT_EQ(history.Record("ignored label", SetOther(doc, 0, 5)), EditHistory::Recorded::InGroup);
    EXPECT_EQ(history.StepCount(), 0u) << "nothing is a step until the group ends";

    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::Committed);
    EXPECT_FALSE(history.InGroup());
    ASSERT_EQ(history.StepCount(), 1u);
    EXPECT_EQ(history.UndoLabel(), "Paste");

    doc.log.clear();
    auto undone = history.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(doc.value, 0);
    EXPECT_EQ(doc.other, 0);
    ASSERT_EQ(undone->effect.entityIds.size(), 2u);
    // Last op first
    EXPECT_EQ(undone->effect.entityIds[0], "other");
    EXPECT_EQ(undone->effect.entityIds[1], "value");

    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_EQ(doc.value, 1);
    EXPECT_EQ(doc.other, 5);
}

TEST(EditHistoryTest, AnEmptyGroupMakesNoStep)
{
    EditHistory history;
    ASSERT_TRUE(history.BeginGroup("Nothing"));
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::Empty);
    EXPECT_EQ(history.StepCount(), 0u);
    EXPECT_FALSE(history.CanUndo());
}

TEST(EditHistoryTest, EndingWithoutAGroupIsNotOpen)
{
    EditHistory history;
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::NotOpen);
    EXPECT_EQ(history.CloseGroups(), EditHistory::GroupEnd::NotOpen);
}

TEST(EditHistoryTest, AGroupInAGroupIsPartOfTheOuterOne)
{
    EditHistory history;
    Doc doc;
    ASSERT_TRUE(history.BeginGroup("Outer"));
    ASSERT_TRUE(history.BeginGroup("Inner"));
    EXPECT_EQ(history.GroupDepth(), 2u);
    doc.value = 1;
    history.Record("x", SetValue(doc, 0, 1));
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::StillOpen);
    EXPECT_TRUE(history.InGroup());
    doc.other = 2;
    history.Record("x", SetOther(doc, 0, 2));
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::Committed);

    ASSERT_EQ(history.StepCount(), 1u);
    EXPECT_EQ(history.UndoLabel(), "Outer");
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 0);
    EXPECT_EQ(doc.other, 0);
}

TEST(EditHistoryTest, GroupsNestOnlySoDeep)
{
    EditHistory history;
    for (size_t i = 0; i < EditHistory::MaxGroupDepth; ++i)
    {
        EXPECT_TRUE(history.BeginGroup("g"));
    }
    EXPECT_FALSE(history.BeginGroup("too deep"));
    EXPECT_EQ(history.GroupDepth(), EditHistory::MaxGroupDepth);
}

TEST(EditHistoryTest, UndoAndRedoAreRefusedInsideAGroup)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);
    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.BeginGroup("Drag"));
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());

    const auto undone = history.Undo();
    ASSERT_FALSE(undone.has_value());
    EXPECT_NE(undone.error().find("group is open"), std::string::npos);
    EXPECT_FALSE(history.Redo().has_value());
    EXPECT_EQ(doc.value, 0) << "a refused undo or redo changes nothing";
}

TEST(EditHistoryTest, CloseGroupsEndsEveryOpenGroupIntoOneStep)
{
    EditHistory history;
    Doc doc;
    history.BeginGroup("Abandoned");
    history.BeginGroup("Inner");
    doc.value = 1;
    history.Record("x", SetValue(doc, 0, 1));
    EXPECT_EQ(history.CloseGroups(), EditHistory::GroupEnd::Committed);
    EXPECT_FALSE(history.InGroup());
    EXPECT_EQ(history.StepCount(), 1u);
    EXPECT_EQ(history.UndoLabel(), "Abandoned");
}

TEST(EditHistoryTest, InAGroupTheSameKeyIsOneOpKeepingTheFirstUndoAndTheLastRedo)
{
    EditHistory history;
    Doc doc;
    history.BeginGroup("Drag");
    for (int i = 1; i <= 100; ++i)
    {
        doc.value = i;
        history.Record("move", SetValue(doc, i - 1, i, "transform:a"));
    }
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::Committed);
    EXPECT_EQ(history.StepCount(), 1u);
    // 100 frames hold one before and one after
    EXPECT_LT(history.TotalBytes(), 1000u);

    doc.log.clear();
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 0);
    ASSERT_EQ(doc.log.size(), 1u);
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_EQ(doc.value, 100);
    EXPECT_EQ(doc.log.size(), 2u);
}

TEST(EditHistoryTest, InAGroupOnlyACommutingOpMergesPastOthers)
{
    EditHistory history;
    Doc doc;
    history.BeginGroup("Drag two");
    // Two objects dragged together: a, b, a, b ... commute, so each is one op
    for (int i = 1; i <= 10; ++i)
    {
        EditOp first = SetValue(doc, i - 1, i, "transform:a");
        first.commutes = true;
        history.Record("move", std::move(first));
        EditOp second = SetOther(doc, i - 1, i, "transform:b");
        second.commutes = true;
        history.Record("move", std::move(second));
    }
    history.EndGroup();
    ASSERT_EQ(history.StepCount(), 1u);
    // Two ops, not twenty: the entry's bytes show it (BytesOf counts a fixed cost per op)
    EXPECT_LT(history.Entries()[0].bytes, 3 * 200u);

    // A non-commuting op in between stops the merge
    EditHistory strict;
    strict.BeginGroup("Mixed");
    strict.Record("a", SetValue(doc, 0, 1, "k"));
    strict.Record("b", SetOther(doc, 0, 1, "other"));
    strict.Record("a again", SetValue(doc, 1, 2, "k"));
    strict.EndGroup();
    ASSERT_EQ(strict.StepCount(), 1u);
    EXPECT_GT(strict.Entries()[0].bytes, 3 * 128u);
}

// ==================== Limits ====================

TEST(EditHistoryTest, TheOldestStepsAreDroppedPastMaxSteps)
{
    EditHistory history;
    history.SetLimits(3, 1024 * 1024);
    Doc doc;
    for (int i = 1; i <= 5; ++i)
    {
        Edit(history, doc, "step " + std::to_string(i), i);
    }
    EXPECT_EQ(history.StepCount(), 3u);
    const auto entries = history.Entries();
    EXPECT_EQ(entries.front().label, "step 3");
    EXPECT_EQ(entries.back().label, "step 5");

    EXPECT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.Undo().has_value());
    EXPECT_FALSE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 2) << "step 1 and 2 can no longer be undone";
}

TEST(EditHistoryTest, TheDefaultCapIs200Steps)
{
    EditHistory history;
    Doc doc;
    for (int i = 1; i <= 250; ++i)
    {
        Edit(history, doc, "step " + std::to_string(i), i);
    }
    EXPECT_EQ(history.StepCount(), 200u);
    EXPECT_EQ(history.Entries().front().label, "step 51");
}

TEST(EditHistoryTest, TheOldestStepsAreDroppedPastMaxBytes)
{
    EditHistory history;
    history.SetLimits(1000, 10 * 1024);
    Doc doc;
    // Each step holds about 2 x 3000 bytes
    for (int i = 1; i <= 5; ++i)
    {
        Edit(history, doc, "snap " + std::to_string(i), i, {}, T0, 3000);
    }
    EXPECT_LE(history.TotalBytes(), 10u * 1024u);
    EXPECT_EQ(history.StepCount(), 1u);
    EXPECT_EQ(history.UndoLabel(), "snap 5");
}

TEST(EditHistoryTest, TheNewestStepIsKeptEvenWhenItAloneIsOverTheCap)
{
    EditHistory history;
    history.SetLimits(10, 100);
    Doc doc;
    Edit(history, doc, "huge", 1, {}, T0, 100000);
    EXPECT_EQ(history.StepCount(), 1u);
    EXPECT_TRUE(history.CanUndo());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_EQ(doc.value, 0);
}

TEST(EditHistoryTest, LoweringTheLimitsDropsWhatIsOver)
{
    EditHistory history;
    Doc doc;
    for (int i = 1; i <= 6; ++i)
    {
        Edit(history, doc, "step " + std::to_string(i), i);
    }
    history.SetLimits(2, 1024 * 1024);
    EXPECT_EQ(history.StepCount(), 2u);
    EXPECT_EQ(history.Entries().front().label, "step 5");
}

TEST(EditHistoryTest, TheLimitsHaveAFloorOfOne)
{
    EditHistory history;
    history.SetLimits(0, 0);
    EXPECT_EQ(history.GetMaxSteps(), 1u);
    EXPECT_EQ(history.GetMaxBytes(), 1u);
}

// ==================== The saved state ====================

TEST(EditHistoryTest, UndoingBackToTheSavedStateIsAtTheSavedState)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);
    history.MarkSaved();
    EXPECT_TRUE(history.IsAtSavedState());

    Edit(history, doc, "two", 2);
    EXPECT_FALSE(history.IsAtSavedState());
    Edit(history, doc, "three", 3);
    EXPECT_FALSE(history.IsAtSavedState());

    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_FALSE(history.IsAtSavedState());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.IsAtSavedState());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_FALSE(history.IsAtSavedState()) << "before the saved state";
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_TRUE(history.IsAtSavedState());
}

TEST(EditHistoryTest, TheEmptyHistoryCanBeTheSavedState)
{
    EditHistory history;
    Doc doc;
    history.MarkSaved();
    EXPECT_TRUE(history.IsAtSavedState());
    Edit(history, doc, "one", 1);
    EXPECT_FALSE(history.IsAtSavedState());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.IsAtSavedState());
}

TEST(EditHistoryTest, ANewEditDiscardingTheSavedStateLosesIt)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);
    Edit(history, doc, "two", 2);
    history.MarkSaved();
    ASSERT_TRUE(history.Undo().has_value());

    // The branch replaces "two", the saved state
    Edit(history, doc, "branch", 7);
    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.Redo().has_value());
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_FALSE(history.IsAtSavedState());
}

TEST(EditHistoryTest, EvictingTheStepsThatLedToTheSavedStateLosesIt)
{
    EditHistory history;
    history.SetLimits(2, 1024 * 1024);
    Doc doc;
    history.MarkSaved();
    Edit(history, doc, "one", 1);
    Edit(history, doc, "two", 2);
    EXPECT_FALSE(history.IsAtSavedState());
    Edit(history, doc, "three", 3); // evicts "one": the empty state can't be reached any more
    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_FALSE(history.IsAtSavedState());
}

TEST(EditHistoryTest, ASavedStateAfterEvictionStillCounts)
{
    EditHistory history;
    history.SetLimits(2, 1024 * 1024);
    Doc doc;
    Edit(history, doc, "one", 1);
    Edit(history, doc, "two", 2);
    history.MarkSaved();
    Edit(history, doc, "three", 3); // evicts "one"; the saved state is after "two"
    EXPECT_FALSE(history.IsAtSavedState());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_TRUE(history.IsAtSavedState());
}

TEST(EditHistoryTest, ForgetSavedAndClearLoseTheSavedState)
{
    EditHistory history;
    history.MarkSaved();
    history.ForgetSaved();
    EXPECT_FALSE(history.IsAtSavedState());

    history.MarkSaved();
    EXPECT_TRUE(history.IsAtSavedState());
    history.Clear();
    EXPECT_FALSE(history.IsAtSavedState());
    history.MarkSaved();
    EXPECT_TRUE(history.IsAtSavedState());
}

// ==================== Clear and failures ====================

TEST(EditHistoryTest, ClearForgetsEverythingAndAnOpenGroup)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);
    ASSERT_TRUE(history.Undo().has_value());
    history.BeginGroup("open");
    history.Record("x", SetValue(doc, 0, 5));
    history.Clear();

    EXPECT_EQ(history.StepCount(), 0u);
    EXPECT_EQ(history.TotalBytes(), 0u);
    EXPECT_FALSE(history.InGroup());
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::NotOpen);
}

TEST(EditHistoryTest, AFailingUndoClearsTheHistoryAndSaysSo)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "one", 1);

    EditOp broken;
    broken.undo = []() -> EditOutcome { return std::unexpected(std::string("the object is gone")); };
    broken.redo = []() -> EditOutcome { return EditEffect{}; };
    history.Record("Broken", std::move(broken));
    ASSERT_EQ(history.StepCount(), 2u);

    const auto undone = history.Undo();
    ASSERT_FALSE(undone.has_value());
    EXPECT_NE(undone.error().find("Couldn't undo 'Broken'"), std::string::npos) << undone.error();
    EXPECT_NE(undone.error().find("the object is gone"), std::string::npos);
    EXPECT_NE(undone.error().find("cleared"), std::string::npos);
    EXPECT_EQ(history.StepCount(), 0u);
    EXPECT_FALSE(history.IsAtSavedState());
}

TEST(EditHistoryTest, AThrowingRedoIsAFailureToo)
{
    EditHistory history;
    EditOp throwing;
    throwing.undo = []() -> EditOutcome { return EditEffect{}; };
    throwing.redo = []() -> EditOutcome { throw std::runtime_error("boom"); };
    history.Record("Throws", std::move(throwing));
    ASSERT_TRUE(history.Undo().has_value());

    const auto redone = history.Redo();
    ASSERT_FALSE(redone.has_value());
    EXPECT_NE(redone.error().find("Couldn't redo 'Throws'"), std::string::npos) << redone.error();
    EXPECT_NE(redone.error().find("boom"), std::string::npos);
    EXPECT_EQ(history.StepCount(), 0u);
}

TEST(EditHistoryTest, AFullEffectSurvivesTheStep)
{
    EditHistory history;
    EditOp restore;
    restore.undo = []() -> EditOutcome
    {
        EditEffect effect;
        effect.full = true;
        return effect;
    };
    restore.redo = []() -> EditOutcome { return Touched("a"); };
    history.Record("Destroy", std::move(restore));

    const auto undone = history.Undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_TRUE(undone->effect.full);
    const auto redone = history.Redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_FALSE(redone->effect.full);
}

// ==================== Review fixes ====================

TEST(EditHistoryTest, ASaveInsideAGroupLeavesNoStateSaved)
{
    EditHistory history;
    Doc doc;
    history.BeginGroup("Drag");
    doc.value = 1;
    history.Record("move", SetValue(doc, 0, 1));
    history.MarkSaved(); // the state the group will lead to isn't known yet
    doc.other = 2;
    history.Record("move", SetOther(doc, 0, 2));
    EXPECT_EQ(history.EndGroup(), EditHistory::GroupEnd::Committed);

    EXPECT_FALSE(history.IsAtSavedState());
    ASSERT_TRUE(history.Undo().has_value());
    EXPECT_FALSE(history.IsAtSavedState()) << "the empty state before the group isn't the saved one either";
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_FALSE(history.IsAtSavedState());
}

TEST(EditHistoryTest, AnOpenGroupsBytesAreCountedAndMergesReplaceTheRedoSide)
{
    EditHistory history;
    Doc doc;
    EXPECT_EQ(history.GroupBytes(), 0u);
    history.BeginGroup("Big");
    history.Record("a", SetValue(doc, 0, 1, "", 1000));
    const size_t one = history.GroupBytes();
    EXPECT_GE(one, 2000u);
    history.Record("b", SetOther(doc, 0, 1));
    EXPECT_GT(history.GroupBytes(), one);

    // The same key again keeps one before and one after: the bytes follow the redo that was replaced
    EditHistory merged;
    merged.BeginGroup("Drag");
    merged.Record("m", SetValue(doc, 0, 1, "k", 500));
    const size_t first = merged.GroupBytes();
    merged.Record("m", SetValue(doc, 1, 2, "k", 700));
    EXPECT_EQ(merged.GroupBytes(), first + 200u);
    merged.EndGroup();
    EXPECT_EQ(merged.GroupBytes(), 0u);
    history.Clear();
    EXPECT_EQ(history.GroupBytes(), 0u);
}

TEST(EditHistoryTest, AStepThatWasUndoneAndRedoneIsNotMergedInto)
{
    EditHistory history;
    Doc doc;
    Edit(history, doc, "Set", 1, "k", T0);
    ASSERT_TRUE(history.Undo().has_value());
    ASSERT_TRUE(history.Redo().has_value());
    EXPECT_EQ(Edit(history, doc, "Set", 2, "k", T0 + 10ms), EditHistory::Recorded::NewStep);
    EXPECT_EQ(history.StepCount(), 2u);
}

TEST(EditHistoryTest, AnInexactEffectSurvivesMerging)
{
    EditEffect first;
    EditEffect second;
    second.inexact = true;
    first.Merge(std::move(second));
    EXPECT_TRUE(first.inexact);
}
