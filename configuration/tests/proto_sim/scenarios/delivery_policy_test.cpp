// The decisions of hub->node command delivery (DeliveryPolicy.h): which first
// wait, what to do when a wait elapses, what giving up does, which acks count.
// Integration with the listener is pinned by TrackedOpRetry / OpRetry / AckPath /
// OptimisticShot / NightlySleep in real_lora_client_test.cpp, which run unchanged
// against the extraction.

#include <gtest/gtest.h>

#include "DeliveryPolicy.h"

using namespace deliverypolicy;

namespace {
constexpr uint8_t kMax = 4;

TimeoutState st(bool awaiting, bool relogin, bool shortw, uint8_t spent) {
    return TimeoutState{awaiting, relogin, shortw, spent, kMax};
}
}  // namespace

// ---- first wait ------------------------------------------------------------

TEST(DeliveryPolicy, TheShortFirstWaitIsOnlyForAnOptimisticFirstShotNeverSpentAndUncharged) {
    EXPECT_TRUE(isShortFirstWait(true, 0, false));
    EXPECT_FALSE(isShortFirstWait(false, 0, false)) << "not an optimistic shot";
    EXPECT_FALSE(isShortFirstWait(true, 1, false)) << "a retry was already spent";
    EXPECT_FALSE(isShortFirstWait(true, 0, true)) << "once per command";
}

// ---- timeout decision --------------------------------------------------------

TEST(DeliveryPolicy, ATimerThatOutlivedItsCommandIsStaleWhateverElseIsTrue) {
    for (int relogin = 0; relogin < 2; ++relogin)
        for (int shortw = 0; shortw < 2; ++shortw)
            for (uint8_t spent : {0, 3, 4, 9}) {
                const TimeoutDecision d = onAckTimeout(st(false, relogin, shortw, spent));
                EXPECT_EQ(d.action, TimeoutAction::Stale);
                EXPECT_EQ(d.retries_spent, spent) << "a stale timer charges nothing";
            }
}

TEST(DeliveryPolicy, AReloginDefersEvenTheShortWaitAndChargesNothing) {
    for (int shortw = 0; shortw < 2; ++shortw)
        for (uint8_t spent : {0, 2, 4}) {
            const TimeoutDecision d = onAckTimeout(st(true, true, shortw, spent));
            EXPECT_EQ(d.action, TimeoutAction::DeferRelogin);
            EXPECT_EQ(d.retries_spent, spent);
            EXPECT_FALSE(isCountedRetry(d.action));
        }
}

TEST(DeliveryPolicy, AMissedShortWaitBurstsWithoutSpendingTheBudget) {
    for (uint8_t spent : {0, 4}) {   // even with the budget on its last step
        const TimeoutDecision d = onAckTimeout(st(true, false, true, spent));
        EXPECT_EQ(d.action, TimeoutAction::ShortWaitMissed);
        EXPECT_EQ(d.retries_spent, spent);
        EXPECT_FALSE(isCountedRetry(d.action));
    }
}

TEST(DeliveryPolicy, TheCountedLadderIsMaxRetriesRetransmitsThenOneGiveUp) {
    uint8_t spent = 0;
    for (uint8_t i = 1; i <= kMax; ++i) {
        const TimeoutDecision d = onAckTimeout(st(true, false, false, spent));
        EXPECT_EQ(d.action, TimeoutAction::Retransmit) << "expiry " << (int) i;
        EXPECT_EQ(d.retries_spent, i);
        EXPECT_TRUE(isCountedRetry(d.action));
        spent = d.retries_spent;
    }
    const TimeoutDecision last = onAckTimeout(st(true, false, false, spent));
    EXPECT_EQ(last.action, TimeoutAction::GiveUp) << "the (max+1)th expiry";
    EXPECT_EQ(last.retries_spent, kMax + 1) << "the exceeding retry is itself counted";
    EXPECT_TRUE(isCountedRetry(last.action));
}

TEST(DeliveryPolicy, TheBudgetComesFromTheCallerNotAConstant) {
    TimeoutState s{true, false, false, 1, 1};
    EXPECT_EQ(onAckTimeout(s).action, TimeoutAction::GiveUp) << "max 1: the 2nd expiry gives up";
    s.max_retries = 2;
    EXPECT_EQ(onAckTimeout(s).action, TimeoutAction::Retransmit);
    s.max_retries = 0;
    s.retries_spent = 0;
    EXPECT_EQ(onAckTimeout(s).action, TimeoutAction::GiveUp) << "max 0: no retry at all";
}

TEST(DeliveryPolicy, OnlyRetransmitAndGiveUpAreCounted) {
    EXPECT_TRUE(isCountedRetry(TimeoutAction::Retransmit));
    EXPECT_TRUE(isCountedRetry(TimeoutAction::GiveUp));
    EXPECT_FALSE(isCountedRetry(TimeoutAction::Stale));
    EXPECT_FALSE(isCountedRetry(TimeoutAction::DeferRelogin));
    EXPECT_FALSE(isCountedRetry(TimeoutAction::ShortWaitMissed));
}

// ---- Rule 4 ---------------------------------------------------------------

TEST(DeliveryPolicy, Rule4MarksOnlyASingleShotThatIsNotAlreadyMarked) {
    EXPECT_TRUE(marksSingleShotUnacked(true, false));
    EXPECT_FALSE(marksSingleShotUnacked(true, true)) << "recorded once";
    EXPECT_FALSE(marksSingleShotUnacked(false, false)) << "a burst has no exposure to bound";
    EXPECT_FALSE(marksSingleShotUnacked(false, true));
}

// ---- giving up --------------------------------------------------------------

TEST(DeliveryPolicy, AnAmbiguousSilenceKeepsTheSessionAnyOtherTearsItDown) {
    EXPECT_EQ(giveUpAction(true), GiveUpAction::FailKeep);
    EXPECT_EQ(giveUpAction(false), GiveUpAction::FailRelogin);
}

// ---- which acks count ------------------------------------------------------------

TEST(DeliveryPolicy, ARangeAckIsAnyIdFromFirstToLastInclusive) {
    EXPECT_FALSE(msgIdInRange(9, 10, 12));
    EXPECT_TRUE(msgIdInRange(10, 10, 12)) << "first is inclusive";
    EXPECT_TRUE(msgIdInRange(11, 10, 12));
    EXPECT_TRUE(msgIdInRange(12, 10, 12)) << "last is inclusive";
    EXPECT_FALSE(msgIdInRange(13, 10, 12));
    EXPECT_TRUE(msgIdInRange(7, 7, 7)) << "a single id";
    EXPECT_FALSE(msgIdInRange(7, 12, 10)) << "an inverted range matches nothing";
}

TEST(DeliveryPolicy, ASetAckIsAnyMemberAndOnlyTheFirstCountEntriesAreRead) {
    const uint32_t ids[] = {4, 5, 99, 0};
    EXPECT_TRUE(msgIdInSet(ids, 2, 4));
    EXPECT_TRUE(msgIdInSet(ids, 2, 5)) << "the last counted entry";
    EXPECT_FALSE(msgIdInSet(ids, 2, 6));
    EXPECT_FALSE(msgIdInSet(ids, 2, 99)) << "beyond count is stale storage, not a member";
    EXPECT_TRUE(msgIdInSet(ids, 3, 99));
    EXPECT_FALSE(msgIdInSet(ids, 0, 4)) << "an empty set matches nothing";
    EXPECT_FALSE(msgIdInSet(ids, 0, 0));
    EXPECT_FALSE(msgIdInSet(ids, 3, 0)) << "0 is 'no id' and is not a member unless counted";
}

TEST(DeliveryPolicy, ARangeAckCountsOnlyWhileTheCommandIsAwaited) {
    EXPECT_TRUE(acceptsRangeAck(true, 11, 10, 12));
    EXPECT_FALSE(acceptsRangeAck(false, 11, 10, 12)) << "not awaiting: an in-range ack is no event";
    EXPECT_FALSE(acceptsRangeAck(true, 13, 10, 12));
    EXPECT_FALSE(acceptsRangeAck(true, 9, 10, 12));
}

TEST(DeliveryPolicy, TheIndexOfAnIdIsItsPositionAmongTheCountedEntriesOrMinusOne) {
    const uint32_t ids[] = {4, 5, 99, 0};
    EXPECT_EQ(msgIdIndex(ids, 3, 4), 0);
    EXPECT_EQ(msgIdIndex(ids, 3, 99), 2);
    EXPECT_EQ(msgIdIndex(ids, 2, 99), -1) << "beyond count is stale storage";
    EXPECT_EQ(msgIdIndex(ids, 0, 4), -1);
}

TEST(AckIds, EveryIdAddedIsFoundWithTheTagOfItsOwnAttempt) {
    AckIds s;
    s.add(10, 0xAAAA);
    s.add(11, 0xBBBB);
    s.add(12, 0xAAAA);
    ASSERT_EQ(s.n, 3);
    ASSERT_GE(s.find(10), 0);
    EXPECT_EQ(s.tag[s.find(10)], 0xAAAAu);
    EXPECT_EQ(s.tag[s.find(11)], 0xBBBBu) << "a late ack applies what ITS attempt carried";
    EXPECT_EQ(s.tag[s.find(12)], 0xAAAAu);
    EXPECT_EQ(s.find(13), -1);
    EXPECT_EQ(s.last(), 12u);
}

TEST(AckIds, ZeroIsNoIdAndIsNeverStoredOrFound) {
    AckIds s;
    s.add(0, 7);
    EXPECT_EQ(s.n, 0);
    EXPECT_EQ(s.find(0), -1);
    EXPECT_EQ(s.last(), 0u) << "an empty set has no latest id";
}

TEST(AckIds, ClearForgetsEverything) {
    AckIds s;
    s.add(1, 1);
    s.add(2, 2);
    s.clear();
    EXPECT_EQ(s.n, 0);
    EXPECT_EQ(s.find(1), -1);
    EXPECT_EQ(s.last(), 0u);
    s.add(3, 3);
    EXPECT_EQ(s.find(3), 0) << "storage is reused from the start";
}

TEST(AckIds, AFullSetForgetsItsOldestAndKeepsTheTagsAligned) {
    AckIds s;
    for (uint32_t i = 1; i <= AckIds::kCap; ++i)
        s.add(i, 100 + i);
    ASSERT_EQ(s.n, AckIds::kCap);
    ASSERT_GE(s.find(1), 0) << "all eight fit";
    s.add(50, 150);
    EXPECT_EQ(s.n, AckIds::kCap) << "bounded";
    EXPECT_EQ(s.find(1), -1) << "the oldest went";
    ASSERT_GE(s.find(2), 0);
    EXPECT_EQ(s.tag[s.find(2)], 102u) << "tags moved with their ids";
    ASSERT_GE(s.find(8), 0);
    EXPECT_EQ(s.tag[s.find(8)], 108u);
    EXPECT_EQ(s.last(), 50u);
    EXPECT_EQ(s.tag[s.find(50)], 150u);
}

TEST(DeliveryPolicy, ALadderOwesItsPushOnlyWhileItIsLiveAndHasARetryLeft) {
    EXPECT_TRUE(ladderHasRetryLeft(true, 0, 3));
    EXPECT_TRUE(ladderHasRetryLeft(true, 2, 3)) << "one retry left";
    EXPECT_FALSE(ladderHasRetryLeft(true, 3, 3)) << "every retry spent: waiting out the last one";
    EXPECT_FALSE(ladderHasRetryLeft(true, 4, 3));
    EXPECT_FALSE(ladderHasRetryLeft(false, 0, 3)) << "acked or never started";
}

TEST(DeliveryPolicy, TheSchedulePolicyIsTodaysLadderAndKeepsTheSessionWhenItFails) {
    EXPECT_EQ(kSchedulePolicy.max_retries, 3);
    EXPECT_EQ(kSchedulePolicy.base_wait_ms, 5000u);
    EXPECT_TRUE(kSchedulePolicy.mode == Mode::Acked);
    EXPECT_TRUE(kSchedulePolicy.resend == Resend::Rebuild) << "a resend carries the latest schedule";
    EXPECT_TRUE(kSchedulePolicy.give_up == GiveUpAction::FailKeep) << "a failed push never tears the session down";
    EXPECT_TRUE(kSchedulePolicy.supersedes_same_kind);
    EXPECT_TRUE(kSchedulePolicy.yields_warmup);
    EXPECT_FALSE(kSchedulePolicy.at_most_once);
}

// ---- the Control channel's four policies (step 5) ---------------------------

TEST(DeliveryPolicy, AllFourControlPoliciesShareTheVerifiedLadder) {
    for (const Policy *p : {&kCoverPolicy, &kSysopPolicy, &kSleepPolicy, &kOtaPolicy}) {
        EXPECT_EQ(p->max_retries, 4);
        EXPECT_EQ(p->base_wait_ms, 3000u);
        EXPECT_TRUE(p->mode == Mode::Acked);
        EXPECT_TRUE(p->resend == Resend::SameBytes) << "a retry re-sends the stored bytes, same msgid";
        EXPECT_TRUE(p->yields_warmup) << "a command stops a Mode B warm-up";
    }
}

TEST(DeliveryPolicy, OnlyACoverOperationIsSupersededByANewerOne) {
    EXPECT_TRUE(kCoverPolicy.supersedes_same_kind);
    EXPECT_FALSE(kSysopPolicy.supersedes_same_kind);
    EXPECT_FALSE(kSleepPolicy.supersedes_same_kind) << "a sleep is never replaced";
    EXPECT_FALSE(kOtaPolicy.supersedes_same_kind) << "an OTA is never replaced";
}

TEST(DeliveryPolicy, OnlyTheSleepKeepsTheSessionWhenItIsGivenUpAndOnlyTheOtaIsAtMostOnce) {
    EXPECT_TRUE(kCoverPolicy.give_up == GiveUpAction::FailRelogin);
    EXPECT_TRUE(kSysopPolicy.give_up == GiveUpAction::FailRelogin);
    EXPECT_TRUE(kOtaPolicy.give_up == GiveUpAction::FailRelogin);
    EXPECT_TRUE(kSleepPolicy.give_up == GiveUpAction::FailKeep);
    EXPECT_TRUE(kOtaPolicy.at_most_once);
    EXPECT_FALSE(kCoverPolicy.at_most_once);
    EXPECT_FALSE(kSysopPolicy.at_most_once);
    EXPECT_FALSE(kSleepPolicy.at_most_once);
}

TEST(DeliveryPolicy, ControlPolicyPicksByKindThenBySysop) {
    constexpr int32_t kSleep = 7, kOta = 9, kOther = 11;
    EXPECT_EQ(&controlPolicy(true, kSleep, kSleep, kOta), &kCoverPolicy) << "a cover op is a cover op whatever the stale sysop field says";
    EXPECT_EQ(&controlPolicy(false, kSleep, kSleep, kOta), &kSleepPolicy);
    EXPECT_EQ(&controlPolicy(false, kOta, kSleep, kOta), &kOtaPolicy);
    EXPECT_EQ(&controlPolicy(false, kOther, kSleep, kOta), &kSysopPolicy);
}

// The policy is pure and constexpr: usable with no listener at all.
static_assert(onAckTimeout(TimeoutState{false, false, false, 0, 4}).action == TimeoutAction::Stale, "");
