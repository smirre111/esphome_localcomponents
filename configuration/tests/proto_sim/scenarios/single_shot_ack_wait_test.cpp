// The first-ack wait of an OPTIMISTIC single shot (SingleShotAckWait.h).
//
// History: the first version was a geometric constant (mark + 500 ms). Node 2
// answers a placed single copy 1.6 - 2.2 s AFTER the mark, so 4 of 8 commands
// were burst on top of an ack that was on its way. The wait is now derived from
// the node's own measured mark->ack delays. These tests pin the policy; the
// integration tests are AdaptiveAckWait.* in real_lora_client_test.cpp.

#include <gtest/gtest.h>

#include "SingleShotAckWait.h"

using namespace singleshotwait;

namespace {
constexpr uint32_t kHour = 3600u * 1000u;
}

// ---- firstAckWaitMs --------------------------------------------------------

TEST(SingleShotAckWait, AMarkAlreadyDueWaitsOnlyTheTail) {
    EXPECT_EQ(firstAckWaitMs(0, 0, 2000), 2000u);
    EXPECT_EQ(firstAckWaitMs(-12345, 0, 2000), 2000u) << "a past mark is not a negative wait";
}

TEST(SingleShotAckWait, TheTimeToTheMarkIsAddedAndRoundedUp) {
    EXPECT_EQ(firstAckWaitMs(1'200'000, 0, 2000), 3200u);
    EXPECT_EQ(firstAckWaitMs(1'200'001, 0, 2000), 3201u) << "rounded UP: never early";
    EXPECT_EQ(firstAckWaitMs(1, 0, 2000), 2001u);
}

TEST(SingleShotAckWait, TheQueueDrainAndTheMarkAreOneInstantNotASum) {
    // The drain already contains the time to the mark for a placed frame;
    // adding both counted the mark twice.
    EXPECT_EQ(firstAckWaitMs(1'000'000, 1'006, 2000), 3006u) << "the later of the two";
    EXPECT_EQ(firstAckWaitMs(1'000'000, 400, 2000), 3000u);
    EXPECT_EQ(firstAckWaitMs(0, 2500, 1000), 3500u) << "a backlog ahead of the frame counts";
}

TEST(SingleShotAckWait, TheFloorIsBackedByTheGeometry) {
    EXPECT_LE(kTailBudgetUs, kFloorTailMs * 1000u);
    EXPECT_GE(kTailBudgetUs, 450'000u) << "dominated by the node's worst pre-CAD backoff";
    EXPECT_LE(kFloorTailMs, kDefaultTailMs);
    EXPECT_LE(kDefaultTailMs, kCeilingTailMs);
}

TEST(SingleShotAckWait, OnlyAnOptimisticFirstShotBeforeAnyRetryUsesTheShortWait) {
    EXPECT_TRUE(useShortFirstWait(true, 0, false));
    EXPECT_FALSE(useShortFirstWait(false, 0, false)) << "a shot the hub was entitled to";
    EXPECT_FALSE(useShortFirstWait(true, 1, false))   << "a retry has been spent";
    EXPECT_FALSE(useShortFirstWait(true, 0, true))    << "once per command";
}

// ---- AckLatency ------------------------------------------------------------

TEST(AckLatency, WithNoSamplesTheTailIsTheConservativeDefault) {
    AckLatency a;
    EXPECT_EQ(a.count(123456), 0u);
    EXPECT_EQ(a.tailMs(123456), kDefaultTailMs);
    EXPECT_EQ(a.tailMs(0), kDefaultTailMs);
    EXPECT_EQ(a.maxDelayMs(0), 0u);
}

TEST(AckLatency, TheTailIsTheSlowestRecentDelayPlusAMargin) {
    AckLatency a;
    a.note(1000, 1900);                   // the slowest is NOT the latest
    a.note(2000, 1650);
    a.note(3000, 1700);
    EXPECT_EQ(a.maxDelayMs(4000), 1900u);
    EXPECT_EQ(a.tailMs(4000), 1900u + 475u) << "25 %";
}

TEST(AckLatency, TheFloorDominatesBelowAboutEightHundredMs) {
    AckLatency a;
    a.note(0, 700);                       // 700 * 1.25 = 875 -> floor
    EXPECT_EQ(a.tailMs(1), kFloorTailMs);
    AckLatency b;
    b.note(0, 810);                       // 1012
    EXPECT_EQ(b.tailMs(1), 1012u);
    AckLatency c;
    c.note(0, 1000);
    EXPECT_EQ(c.tailMs(1), 1250u);
}

TEST(AckLatency, TheTailNeverUndercutsTheObservedLatencyOnceAboveTheFloor) {
    for (uint32_t d = 0; d <= 3000; d += 50) {
        AckLatency a;
        a.note(10, d);
        const uint32_t t = a.tailMs(11);
        EXPECT_GE(t, d < kFloorTailMs ? kFloorTailMs : (d > kCeilingTailMs ? kCeilingTailMs : d));
        EXPECT_GE(t, d <= kCeilingTailMs ? d : kCeilingTailMs) << "delay " << d;
        EXPECT_LE(t, kCeilingTailMs);
        EXPECT_GE(t, kFloorTailMs);
    }
}

TEST(AckLatency, ClampsAtTheCeilingAndAFreakValueDoesNoMoreThanThat) {
    AckLatency a;
    a.note(0, 2900);
    EXPECT_EQ(a.tailMs(1), kCeilingTailMs) << "2900 + 725 clamps";
    AckLatency b;
    EXPECT_TRUE(b.note(0, 9000));         // a late retry's ack: kept, clamped
    EXPECT_EQ(b.maxDelayMs(1), kCeilingTailMs);
    EXPECT_EQ(b.tailMs(1), kCeilingTailMs);
}

TEST(AckLatency, ImplausibleDelaysAreDroppedNotClamped) {
    AckLatency a;
    EXPECT_FALSE(a.note(0, kPlausibleMaxDelayMs + 1)) << "a stale mark is not a latency";
    EXPECT_EQ(a.count(1), 0u);
    EXPECT_EQ(a.tailMs(1), kDefaultTailMs);
    EXPECT_TRUE(a.note(0, kPlausibleMaxDelayMs));
    EXPECT_EQ(a.count(1), 1u);
}

TEST(AckLatency, AFastNodeGetsAShorterDeadlineThanTheCeiling) {
    AckLatency a;
    for (int i = 0; i < 4; ++i) a.note(100 * i, 1100);
    EXPECT_LT(a.tailMs(1000), kCeilingTailMs);
    EXPECT_EQ(a.tailMs(1000), 1375u);
}

TEST(AckLatency, TheWindowIsRollingOldestSampleIsReplaced) {
    AckLatency a;
    a.note(0, 2800);                      // the slow outlier
    EXPECT_EQ(a.maxDelayMs(1), 2800u);
    for (unsigned i = 0; i < kSamples; ++i) a.note(10 + i, 1200);
    EXPECT_EQ(a.count(100), kSamples);
    EXPECT_EQ(a.maxDelayMs(100), 1200u) << "after kSamples newer ones the outlier is gone";
    EXPECT_EQ(a.tailMs(100), 1500u);
}

TEST(AckLatency, SamplesAgeOut) {
    AckLatency a;
    a.note(0, 2500);
    EXPECT_EQ(a.count(kSampleMaxAgeMs), 1u) << "exactly at the limit still counts";
    EXPECT_EQ(a.count(kSampleMaxAgeMs + 1), 0u);
    EXPECT_EQ(a.tailMs(kSampleMaxAgeMs + 1), kDefaultTailMs) << "back to the default";
    a.note(kSampleMaxAgeMs, 1000);        // a fresh one next to the stale one
    EXPECT_EQ(a.count(kSampleMaxAgeMs + 1), 1u);
    EXPECT_EQ(a.maxDelayMs(kSampleMaxAgeMs + 1), 1000u) << "the stale 2500 is ignored";
}

TEST(AckLatency, AgeingSurvivesTheMillisecondClockWrapping) {
    AckLatency a;
    const uint32_t before_wrap = 0xFFFFFFFFu - 1000u;
    a.note(before_wrap, 1500);
    EXPECT_EQ(a.count(before_wrap + 2000u), 1u) << "2 s later, across the wrap";
    EXPECT_EQ(a.count(before_wrap + kSampleMaxAgeMs + 1u), 0u);
}

TEST(AckLatency, ThirtyTwoNodesDoNotShareALatency) {
    AckLatency nodes[32];
    for (int i = 0; i < 32; ++i)
        if (i % 2) nodes[i].note(0, 2400);            // odd nodes are slow
        else       nodes[i].note(0, 1100);            // even nodes are fast
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(nodes[i].count(10), 1u);
        EXPECT_EQ(nodes[i].tailMs(10), i % 2 ? 3000u : 1375u) << "node " << i;
    }
    // One node learning a miss does not move the others.
    nodes[4].note(20, 1375);
    nodes[4].note(20, 1719);
    EXPECT_GT(nodes[4].tailMs(30), 1375u);
    EXPECT_EQ(nodes[6].tailMs(30), 1375u);
    EXPECT_EQ(nodes[5].tailMs(30), 3000u);
}

TEST(AckLatency, ATailOnTheMissPathRampsTheNextDeadlineUp) {
    // What the caller does on a miss: note the tail that was too short.
    AckLatency a;
    a.note(0, 1100);
    uint32_t tail = a.tailMs(1);
    uint32_t prev = 0;
    for (int i = 0; i < 12 && tail < kCeilingTailMs; ++i) {
        prev = tail;
        a.note(1, tail);
        tail = a.tailMs(1);
        EXPECT_GT(tail, prev) << "each miss lengthens the next wait";
    }
    EXPECT_EQ(tail, kCeilingTailMs) << "and it stops at the normal wait";
}

// ---- isCleanSample ---------------------------------------------------------

TEST(SingleShotAckWait, OnlyACleanSinglePlacedCopyIsASample) {
    EXPECT_TRUE(isCleanSample(true, 0, false, true));
    EXPECT_FALSE(isCleanSample(false, 0, false, true)) << "a burst: deferred ack";
    EXPECT_FALSE(isCleanSample(true, 1, false, true))  << "a retry was spent";
    EXPECT_FALSE(isCleanSample(true, 0, true, true))   << "a fallback burst blinds the hub";
    EXPECT_FALSE(isCleanSample(true, 0, false, false)) << "no mark, no delay";
}

static_assert(kHour == 3600000u, "sanity");
