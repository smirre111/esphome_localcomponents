// TxQueue — the reordering, time-scheduled transmit queue (B1a).
//
// The gate is one sentence: "a frame can be placed 'not before round n+2, and
// behind nothing else'". Both halves are ordering statements a FIFO cannot
// express, so the tests are about ORDER, not throughput.
//
// See docs/implementation-plan.md 4.5 and section 8's B1a row.

#include <gtest/gtest.h>

#include <vector>

#include "TxQueue.h"
#include "TimedGrid.h"

using namespace txqueue;

namespace {
std::vector<uint8_t> drain(Queue &q, int64_t now) {
    std::vector<uint8_t> out;
    for (;;) {
        const uint8_t s = q.pop(now);
        if (s == kInvalidSlot) break;
        out.push_back(s);
    }
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// FIFO is preserved where nothing asks otherwise
// ---------------------------------------------------------------------------

TEST(TxQueue, EqualPriorityKeepsInsertionOrder) {
    // Not a detail: a command and its retry are built in order, and the retry
    // ladder assumes they arrive in that order. Without a stable tiebreak the
    // second could overwrite the first at the far end.
    Queue q;
    for (uint8_t i = 0; i < 5; ++i)
        ASSERT_TRUE(q.push(i, 0, Priority::Normal));

    EXPECT_EQ(drain(q, 0), (std::vector<uint8_t>{0, 1, 2, 3, 4}));
}

TEST(TxQueue, AnEmptyQueueYieldsNothing) {
    Queue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.pop(0), kInvalidSlot);
}

// ---------------------------------------------------------------------------
// "not before round n+2"
// ---------------------------------------------------------------------------

TEST(TxQueue, ADeferredFrameIsNotEligibleEarly) {
    Queue q;
    const int64_t now = 1000000;
    const int64_t later = deferUntilUs(now, timedgrid::kRoundUs);
    ASSERT_TRUE(q.push(7, later, Priority::Normal));

    EXPECT_EQ(q.pop(now), kInvalidSlot) << "not before means not before";
    EXPECT_EQ(q.pop(later - 1), kInvalidSlot);
    EXPECT_EQ(q.pop(later), 7u);
}

TEST(TxQueue, DeferralIsTwoRoundsNotOne) {
    // A burst occupies 1450 ms and sendTask blocks a further 400 ms — 1850 ms
    // against a 1500 ms round — so one round does not clear it.
    const int64_t now = 0;
    const int64_t until = deferUntilUs(now, timedgrid::kRoundUs);
    EXPECT_EQ(until, 2 * (int64_t) timedgrid::kRoundUs);
    EXPECT_GT(until, 1850000) << "must clear a burst plus its response window";
}

TEST(TxQueue, NotEligibleIsNotTheSameAsEmpty) {
    // A caller that treated them alike would spin at 100 % CPU on a queue that
    // holds only deferred frames.
    Queue q;
    ASSERT_TRUE(q.push(3, 5000, Priority::Normal));
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.pop(0), kInvalidSlot);
    EXPECT_EQ(q.nextEligibleUs(0), 5000);
}

TEST(TxQueue, NextEligibleReportsNowWhenSomethingIsReady) {
    Queue q;
    ASSERT_TRUE(q.push(1, 0, Priority::Normal));
    ASSERT_TRUE(q.push(2, 900000, Priority::Normal));
    EXPECT_EQ(q.nextEligibleUs(100), 100) << "something is ready right now";
}

TEST(TxQueue, NextEligibleOnAnEmptyQueueIsNever) {
    Queue q;
    EXPECT_EQ(q.nextEligibleUs(0), INT64_MAX);
}

// ---------------------------------------------------------------------------
// "behind nothing else"
// ---------------------------------------------------------------------------

TEST(TxQueue, AnImmediateFrameJumpsEveryEligibleFrame) {
    // The other half of the gate: having waited its two rounds, the deferred
    // frame must not then queue behind traffic that arrived while it waited.
    Queue q;
    for (uint8_t i = 0; i < 4; ++i)
        ASSERT_TRUE(q.push(i, 0, Priority::Normal));
    ASSERT_TRUE(q.push(99, 0, Priority::Immediate));

    EXPECT_EQ(q.pop(0), 99u) << "behind nothing else";
    EXPECT_EQ(drain(q, 0), (std::vector<uint8_t>{0, 1, 2, 3}))
        << "and the rest keep their order";
}

TEST(TxQueue, PriorityDoesNotOverrideTheDeferral) {
    // Highest priority still cannot go before its earliest instant — otherwise
    // "not before round n+2" would be silently defeated by raising priority.
    Queue q;
    ASSERT_TRUE(q.push(5, 1000, Priority::Immediate));
    ASSERT_TRUE(q.push(6, 0, Priority::Background));

    EXPECT_EQ(q.pop(0), 6u) << "the only ELIGIBLE frame wins, whatever its rank";
    EXPECT_EQ(q.pop(1000), 5u);
}

TEST(TxQueue, BackgroundTrafficYieldsToEverything) {
    Queue q;
    ASSERT_TRUE(q.push(1, 0, Priority::Background));   // queued first
    ASSERT_TRUE(q.push(2, 0, Priority::Normal));
    ASSERT_TRUE(q.push(3, 0, Priority::Immediate));
    EXPECT_EQ(drain(q, 0), (std::vector<uint8_t>{3, 2, 1}));
}

TEST(TxQueue, EqualPriorityIsNotReorderedByEarliestInstant) {
    // Two frames that became eligible at different times but share a priority
    // must still go out in the order they were BUILT.
    Queue q;
    ASSERT_TRUE(q.push(1, 500, Priority::Normal));   // built first, ready later
    ASSERT_TRUE(q.push(2, 0, Priority::Normal));     // built second, ready now

    EXPECT_EQ(q.pop(0), 2u) << "only one is eligible at t=0";
    EXPECT_EQ(q.pop(1000), 1u);

    Queue r;
    ASSERT_TRUE(r.push(1, 0, Priority::Normal));
    ASSERT_TRUE(r.push(2, 0, Priority::Normal));
    EXPECT_EQ(drain(r, 1000), (std::vector<uint8_t>{1, 2}))
        << "both eligible: insertion order, never earliest_us";
}

// ---------------------------------------------------------------------------
// Capacity and hygiene
// ---------------------------------------------------------------------------

TEST(TxQueue, AFullQueueRefusesRatherThanDropsSilently) {
    // The caller keeps its buffer and can log. Silently discarding a downlink
    // is how a command disappears with no trace.
    Queue q;
    for (uint8_t i = 0; i < kMaxEntries; ++i)
        ASSERT_TRUE(q.push(i, 0, Priority::Normal)) << i;
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.push(100, 0, Priority::Immediate))
        << "even the highest priority cannot displace a queued frame";
}

TEST(TxQueue, SpaceIsReusedAfterAPop) {
    Queue q;
    for (uint8_t i = 0; i < kMaxEntries; ++i) ASSERT_TRUE(q.push(i, 0, Priority::Normal));
    ASSERT_EQ(q.pop(0), 0u);
    EXPECT_FALSE(q.full());
    EXPECT_TRUE(q.push(50, 0, Priority::Normal));
}

TEST(TxQueue, NothingIsDeliveredTwice) {
    Queue q;
    for (uint8_t i = 0; i < 6; ++i) ASSERT_TRUE(q.push(i, 0, Priority::Normal));
    const auto got = drain(q, 0);
    EXPECT_EQ(got.size(), 6u);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.pop(0), kInvalidSlot);
    EXPECT_EQ(q.size(), 0u);
}

TEST(TxQueue, AnInvalidSlotIsRejected) {
    Queue q;
    EXPECT_FALSE(q.push(kInvalidSlot, 0, Priority::Normal))
        << "kInvalidSlot is the sentinel pop() returns; queueing it would make "
           "a real entry indistinguishable from an empty queue";
}

TEST(TxQueue, ClearEmptiesEverythingIncludingTheSequence) {
    Queue q;
    for (uint8_t i = 0; i < 5; ++i) ASSERT_TRUE(q.push(i, 0, Priority::Normal));
    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.pop(0), kInvalidSlot);

    // Sequence restarts, so ordering after a clear is still insertion order.
    ASSERT_TRUE(q.push(9, 0, Priority::Normal));
    ASSERT_TRUE(q.push(8, 0, Priority::Normal));
    EXPECT_EQ(drain(q, 0), (std::vector<uint8_t>{9, 8}));
}

TEST(TxQueue, AClockThatJumpsBackwardsDoesNotLoseFrames) {
    // This container's clock does exactly that. A frame already eligible must
    // not become permanently ineligible because `now` went backwards.
    Queue q;
    ASSERT_TRUE(q.push(4, 1000, Priority::Normal));
    EXPECT_EQ(q.pop(500), kInvalidSlot);
    EXPECT_EQ(q.pop(2000), 4u) << "it is still there once time passes again";
}

// ---------------------------------------------------------------------------
// Supersession — how a queued frame learns it is no longer wanted
//
// The queue's ordering rule deliberately KEEPS two frames for the same node,
// placing the second at the following mark. That is right for two commands a
// person actually asked for, and wrong for a superseded one: the hub tracks
// exactly one command per node, so the moment a second arrives it has already
// stopped accepting the first's ack and stopped retrying it. Sending it anyway
// is what moved the blind twice, 1.5 s apart, from one gesture.
// ---------------------------------------------------------------------------

TEST(SupersedeTable, ANewerGenerationRetiresAnOlderOne) {
    SupersedeTable t;
    t.note(/*key=*/18, /*gen=*/1);
    EXPECT_TRUE(t.isCurrent(18, 1));

    t.note(18, 2);
    EXPECT_FALSE(t.isCurrent(18, 1)) << "the first command is no longer wanted";
    EXPECT_TRUE(t.isCurrent(18, 2));
}

TEST(SupersedeTable, ARetryUnderTheSameGenerationSurvives) {
    // The retransmit path resends the STORED frame under the generation it was
    // built with. If a retry were given a fresh generation it would supersede
    // the frame it is a retry of — the original bug with an extra step.
    SupersedeTable t;
    t.note(18, 7);
    t.note(18, 7);
    EXPECT_TRUE(t.isCurrent(18, 7));
}

TEST(SupersedeTable, NodesDoNotRetireEachOthersCommands) {
    // The key is the node's short address. A busy fleet queues frames for many
    // nodes between one node's command and its mark, and every one of those
    // would otherwise look like a supersession.
    SupersedeTable t;
    t.note(18, 1);
    t.note(19, 5);
    t.note(20, 9);
    EXPECT_TRUE(t.isCurrent(18, 1));
    EXPECT_TRUE(t.isCurrent(19, 5));
    EXPECT_TRUE(t.isCurrent(20, 9));
}

TEST(SupersedeTable, KeyZeroTakesNoPartInAnyOfThis) {
    // Every frame except a tracked op: beacons, GridSync, TimeSync, schedule
    // pushes. A beacon that could be retired by a command would be a broadcast
    // the whole fleet stopped receiving because one node was told to move.
    SupersedeTable t;
    t.note(0, 1);
    t.note(0, 2);
    EXPECT_TRUE(t.isCurrent(0, 1));
    EXPECT_TRUE(t.isCurrent(0, 0));
}

TEST(SupersedeTable, AnUnknownKeyIsCurrent) {
    // The safe direction is SEND. A frame whose key the table has never seen is
    // either the first for that node or one whose record was lost; dropping it
    // would turn a bookkeeping gap into a lost command.
    SupersedeTable t;
    EXPECT_TRUE(t.isCurrent(42, 1));
}

TEST(SupersedeTable, AnOutOfOrderNoteDoesNotLowerTheMark) {
    // note() is called as frames are ACCEPTED, and nothing guarantees two
    // producers hit it in generation order. Taking the lower of the two would
    // resurrect a command the hub has stopped tracking.
    SupersedeTable t;
    t.note(18, 5);
    t.note(18, 3);
    EXPECT_FALSE(t.isCurrent(18, 3));
    EXPECT_TRUE(t.isCurrent(18, 5));
}

TEST(SupersedeTable, AFullTableFailsTowardsSending) {
    // Sized to the grid, so a full fleet cannot evict each other. Past that the
    // failure mode is deliberately the OLD behaviour — both frames go out —
    // rather than a command silently dropped because a table was full.
    SupersedeTable t;
    for (uint32_t k = 1; k <= SupersedeTable::kMaxKeys; ++k)
        t.note(k, 2);
    const uint32_t overflow = SupersedeTable::kMaxKeys + 1;
    t.note(overflow, 2);
    EXPECT_TRUE(t.isCurrent(overflow, 1))
        << "an unrecorded key must still be transmitted";
    EXPECT_FALSE(t.isCurrent(1, 1)) << "the recorded ones still work";
}
