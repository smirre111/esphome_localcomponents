// The first-ack wait of an OPTIMISTIC single shot (SingleShotAckWait.h).
//
// The arithmetic is tiny and the failure it prevents is subtle: the wait is
// measured from PLACEMENT, and a placed frame can be a whole round away from
// the air, so a flat 500 ms would burst on top of a command that has not been
// sent yet.

#include <gtest/gtest.h>

#include "SingleShotAckWait.h"

using namespace singleshotwait;

TEST(SingleShotAckWait, AMarkAlreadyDueWaitsOnlyTheTail) {
    EXPECT_EQ(firstAckWaitMs(0, 3000), kTailMs);
    EXPECT_EQ(firstAckWaitMs(-12345, 3000), kTailMs) << "a past mark is not a negative wait";
}

TEST(SingleShotAckWait, TheTimeToTheMarkIsAddedAndRoundedUp) {
    EXPECT_EQ(firstAckWaitMs(1'200'000, 3000), 1200u + kTailMs);
    EXPECT_EQ(firstAckWaitMs(1'200'001, 3000), 1201u + kTailMs) << "rounded UP: never early";
    EXPECT_EQ(firstAckWaitMs(1, 3000), 1u + kTailMs);
}

TEST(SingleShotAckWait, ItIsNeverLongerThanTheNormalWait) {
    // A whole round away (1.5 s) plus the tail is 2 s, inside 3 s; anything
    // beyond is capped, because this is an optimisation of the MISS case.
    EXPECT_EQ(firstAckWaitMs(1'500'000, 3000), 2000u);
    EXPECT_EQ(firstAckWaitMs(9'000'000, 3000), 3000u);
    EXPECT_EQ(firstAckWaitMs(0, 300), 300u);
}

TEST(SingleShotAckWait, TheTailCoversEverythingAfterTheMark) {
    // The static_assert in the header is the real guard; restated here so a
    // reader of the test list sees the budget, and so the 500 ms cannot be
    // lowered without this naming what it would stop covering.
    EXPECT_LE(kTailBudgetUs, kTailMs * 1000u);
    EXPECT_GE(kTailBudgetUs, 450'000u) << "the budget is dominated by the node's "
                                          "worst pre-CAD backoff; if it shrank a lot "
                                          "the tail may be loosened";
    EXPECT_EQ(kTailMs, 500u);
}

TEST(SingleShotAckWait, OnlyAnOptimisticFirstShotBeforeAnyRetryUsesTheShortWait) {
    EXPECT_TRUE(useShortFirstWait(true, 0, false));
    EXPECT_FALSE(useShortFirstWait(false, 0, false)) << "a shot the hub was entitled to";
    EXPECT_FALSE(useShortFirstWait(true, 1, false))   << "a retry has been spent";
    EXPECT_FALSE(useShortFirstWait(true, 0, true))    << "once per command";
}
