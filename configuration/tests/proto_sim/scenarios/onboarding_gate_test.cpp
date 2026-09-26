// OnboardingGate — one node's login-to-settled window at a time.
//
// The gate is policy over two things the hub cannot afford to get wrong: WHO may
// begin a login challenge, and WHEN a hold ends. A hold that never ends stalls
// every other node; one that ends too early lets two handshakes interleave on a
// single downlink queue, which is the defect this exists to remove. So both
// halves are pinned here, including the ways a hold can be abandoned.

#include <gtest/gtest.h>

#include "OnboardingGate.h"

using namespace onboarding;

namespace {

int a, b, c;   // three distinct identities; only their addresses matter

HoldState confirmedAndQuiet() {
    HoldState s;
    s.age_ms           = 9000;
    s.confirmed        = true;
    s.since_confirm_ms = kSettleAfterConfirmMs;
    s.node_heard       = true;
    return s;
}

}  // namespace

// ---- the gate ---------------------------------------------------------------

TEST(OnboardingGate, TheFirstAskerGetsAFreeGate) {
    Gate g;
    EXPECT_TRUE(g.tryAcquire(&a));
    EXPECT_TRUE(g.isHolder(&a));
    EXPECT_EQ(g.waiting(), 0u);
}

TEST(OnboardingGate, AHolderAskingAgainKeepsItAndDoesNotQueueBehindItself) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_TRUE(g.tryAcquire(&a));
    EXPECT_EQ(g.waiting(), 0u);
}

TEST(OnboardingGate, ASecondNodeIsRefusedWhileTheFirstHoldsTheGate) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b)) << "two handshakes on one downlink queue is the bug";
    EXPECT_TRUE(g.isHolder(&a)) << "and refusing b must not disturb a";
    EXPECT_EQ(g.waiting(), 1u);
}

TEST(OnboardingGate, AskingRepeatedlyDoesNotQueueTwiceOrLoseAPlace) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(g.tryAcquire(&b));
    EXPECT_EQ(g.waiting(), 1u) << "a poll every 500 ms must not grow the line";
}

TEST(OnboardingGate, ReleaseHandsTheGateToWhoAskedFirstNotWhoAsksNext) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b));   // b asked first
    EXPECT_FALSE(g.tryAcquire(&c));   // c second
    g.release(&a);

    EXPECT_FALSE(g.tryAcquire(&c)) << "c is behind b even though c asks first now";
    EXPECT_TRUE(g.tryAcquire(&b));
    EXPECT_TRUE(g.isHolder(&b));
    EXPECT_EQ(g.waiting(), 1u) << "c is still waiting, and only c";
}

TEST(OnboardingGate, ANodeThatIsNotFirstInLineCannotJumpAFreeGate) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b));
    g.release(&a);
    EXPECT_FALSE(g.tryAcquire(&c)) << "the gate is free, but b was here first";
    EXPECT_FALSE(g.held());
}

TEST(OnboardingGate, AStaleReleaseCannotFreeAnotherNodesHold) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    g.release(&b);   // b never held it: a timer that outlived its owner's hold
    EXPECT_TRUE(g.isHolder(&a));
}

TEST(OnboardingGate, ReleasingWhileOnlyWaitingLeavesTheLine) {
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b));
    g.release(&b);
    EXPECT_EQ(g.waiting(), 0u);
    g.release(&a);
    EXPECT_TRUE(g.tryAcquire(&c)) << "a departed waiter must not keep the gate shut";
}

TEST(OnboardingGate, AFrontOfLineThatStoppedAskingCannotBlockAFreeGateForever) {
    // b queued and was then cancelled without releasing (its login timer was
    // replaced). The gate is free; c is the only live asker.
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b));
    g.release(&a);

    bool granted = false;
    int asks = 0;
    for (; asks < 20 && !granted; ++asks) granted = g.tryAcquire(&c);
    EXPECT_TRUE(granted) << "c must not be starved by a ghost";
    EXPECT_GT(asks, 1) << "but a live front-of-line is given a chance to come back first";
    EXPECT_TRUE(g.isHolder(&c));
}

TEST(OnboardingGate, ALiveFrontOfLineTakesTheGateBeforeAnyoneElseCan) {
    // The patience must not be so short that a live waiter is evicted: between
    // two polls of the front, every OTHER waiter asks once.
    Gate g;
    ASSERT_TRUE(g.tryAcquire(&a));
    EXPECT_FALSE(g.tryAcquire(&b));
    EXPECT_FALSE(g.tryAcquire(&c));
    g.release(&a);

    EXPECT_FALSE(g.tryAcquire(&c)) << "c asks again first, as it would in a busy fleet";
    EXPECT_TRUE(g.tryAcquire(&b)) << "b was not evicted by c's ask";
    EXPECT_TRUE(g.isHolder(&b));
}

TEST(OnboardingGate, ANullIdentityIsNeverGranted) {
    Gate g;
    EXPECT_FALSE(g.tryAcquire(nullptr));
    EXPECT_FALSE(g.held());
}

// ---- when a hold ends -------------------------------------------------------

TEST(OnboardingHold, ANodeStillLoggingInKeepsTheGate) {
    HoldState s;
    s.age_ms = 4000; s.node_heard = true;
    EXPECT_EQ(assess(s), Verdict::Keep);
}

TEST(OnboardingHold, AConfirmedSessionIsNotSettledUntilTheFollowUpPushesAreQueued) {
    HoldState s = confirmedAndQuiet();
    s.since_confirm_ms = kSettleAfterConfirmMs - 1;
    EXPECT_EQ(assess(s), Verdict::Keep)
        << "TimeSync, GridSync and ScheduleConfig are queued at +0.75/+1.25/+2 s; "
           "releasing before them lets the next node's login land in front";
    s.since_confirm_ms = kSettleAfterConfirmMs;
    EXPECT_EQ(assess(s), Verdict::Settled);
}

TEST(OnboardingHold, AConfirmedSessionWithFramesStillQueuedIsNotSettled) {
    HoldState s = confirmedAndQuiet();
    s.tx_drain_us = 4'000'000;
    EXPECT_EQ(assess(s), Verdict::Keep) << "its own pushes are still on the way out";
}

TEST(OnboardingHold, AConfirmedSessionWithAScheduleUnackedIsNotSettled) {
    HoldState s = confirmedAndQuiet();
    s.schedule_outstanding = true;
    EXPECT_EQ(assess(s), Verdict::Keep)
        << "the schedule push and its retries are part of bringing the node up";
}

TEST(OnboardingHold, ANodeThatNeverAnswersReleasesTheGateForTheOthers) {
    HoldState s;
    s.age_ms = kSilentAfterMs;
    EXPECT_EQ(assess(s), Verdict::NodeSilent);
}

TEST(OnboardingHold, SilenceBeforeTheThresholdIsNotYetSilence) {
    HoldState s;
    s.age_ms = kSilentAfterMs - 1;
    EXPECT_EQ(assess(s), Verdict::Keep);
}

TEST(OnboardingHold, ALoginStillQueuedBehindOthersHasNotBeenAskedYetSoIsNotSilent) {
    HoldState s;
    s.age_ms      = kSilentAfterMs + 5000;
    s.tx_drain_us = 6'000'000;
    EXPECT_EQ(assess(s), Verdict::Keep)
        << "blaming the node for the hub's own backlog would release the gate "
           "into exactly the pile-up it exists to prevent";
}

TEST(OnboardingHold, ANodeThatWasHeardIsNotCalledSilentEvenIfItHasNotConfirmed) {
    HoldState s;
    s.age_ms = kSilentAfterMs + 1000;
    s.node_heard = true;
    EXPECT_EQ(assess(s), Verdict::Keep) << "it answered; the login is merely slow";
}

TEST(OnboardingHold, NoHoldOutlastsTheCapWhateverElseIsTrue) {
    HoldState s;
    s.age_ms = kMaxHoldMs;
    s.node_heard = true;
    s.tx_drain_us = 9'000'000;
    s.schedule_outstanding = true;
    EXPECT_EQ(assess(s), Verdict::Capped)
        << "a state nobody anticipated must not park the gate for the whole fleet";

    HoldState c = confirmedAndQuiet();
    c.age_ms = kMaxHoldMs;
    c.schedule_outstanding = true;
    EXPECT_EQ(assess(c), Verdict::Capped);
}

TEST(OnboardingHold, TheCapLeavesRoomForTheLongestRetryLadder) {
    // ScheduleConfig: 3 retries at 5 s plus a backlog. If the cap were shorter
    // than one full ladder, a slow-but-working node would be cut off mid-push.
    EXPECT_GE(kMaxHoldMs, kSilentAfterMs + kSettleAfterConfirmMs + 3 * 5000u);
}
