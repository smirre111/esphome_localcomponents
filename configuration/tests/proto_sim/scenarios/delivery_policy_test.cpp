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

// ---- the Control channel's line (step 5d) ------------------------------------

namespace {
ControlCmd cover(float pos) { return ControlCmd{true, 1, 0, pos, 0}; }
ControlCmd sysop(int32_t s) { return ControlCmd{false, 0, 0, 0.0f, s}; }
}  // namespace

TEST(ControlAdmission, NothingInFlightStartsAtOnceWhateverItIs) {
    EXPECT_TRUE(admitControl(false, false, true) == ControlAdmit::StartNow);
    EXPECT_TRUE(admitControl(false, true, false) == ControlAdmit::StartNow);
    EXPECT_TRUE(admitControl(false, true, true) == ControlAdmit::StartNow);
    EXPECT_TRUE(admitControl(false, false, false) == ControlAdmit::StartNow);
}

TEST(ControlAdmission, OnlyACoverOpReplacesAndOnlyACoverOp) {
    EXPECT_TRUE(admitControl(true, true, true) == ControlAdmit::Replace) << "latest cover op wins";
    EXPECT_TRUE(admitControl(true, false, true) == ControlAdmit::Queue) << "a cover op never cancels a sysop";
    EXPECT_TRUE(admitControl(true, true, false) == ControlAdmit::Queue) << "a sysop never cancels a cover op";
    EXPECT_TRUE(admitControl(true, false, false) == ControlAdmit::Queue) << "a sysop is never superseded";
}

TEST(ControlQueue, IsFirstInFirstOut) {
    ControlQueue q;
    EXPECT_TRUE(q.empty());
    ControlCmd out{};
    EXPECT_FALSE(q.pop(&out)) << "nothing waits";
    ASSERT_TRUE(q.push(sysop(10)) == ControlQueue::Push::Queued);
    ASSERT_TRUE(q.push(sysop(11)) == ControlQueue::Push::Queued);
    ASSERT_TRUE(q.push(sysop(12)) == ControlQueue::Push::Queued);
    EXPECT_EQ(q.size(), 3u);
    for (int32_t want : {10, 11, 12}) {
        ASSERT_TRUE(q.pop(&out));
        EXPECT_EQ(out.sysop, want);
    }
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.pop(&out));
}

TEST(ControlQueue, ARefusedPushDropsTheNewestAndKeepsEveryOlderOne) {
    ControlQueue q;
    for (unsigned i = 0; i < ControlQueue::kDepth; ++i)
        ASSERT_TRUE(q.push(sysop(20 + (int32_t) i)) == ControlQueue::Push::Queued);
    EXPECT_EQ(ControlQueue::kDepth, 4u);
    EXPECT_TRUE(q.push(sysop(99)) == ControlQueue::Push::Full);
    EXPECT_TRUE(q.push(cover(0.5f)) == ControlQueue::Push::Full) << "a cover op with no cover op to replace needs room too";
    EXPECT_EQ(q.size(), ControlQueue::kDepth);
    EXPECT_FALSE(q.hasSysop(99));
    EXPECT_FALSE(q.hasCover());
    ControlCmd out{};
    for (int32_t want : {20, 21, 22, 23}) {
        ASSERT_TRUE(q.pop(&out));
        EXPECT_EQ(out.sysop, want);
    }
}

TEST(ControlQueue, ANewerCoverOpReplacesTheWaitingOneAndMovesToTheBack) {
    ControlQueue q;
    ASSERT_TRUE(q.push(cover(0.1f)) == ControlQueue::Push::Queued);
    ASSERT_TRUE(q.push(sysop(7)) == ControlQueue::Push::Queued);
    EXPECT_TRUE(q.push(cover(0.9f)) == ControlQueue::Push::ReplacedCover);
    EXPECT_EQ(q.size(), 2u) << "never two cover ops waiting";
    ControlCmd out{};
    ASSERT_TRUE(q.pop(&out));
    EXPECT_FALSE(out.is_cover) << "the sysop asked for in between is now first";
    EXPECT_EQ(out.sysop, 7);
    ASSERT_TRUE(q.pop(&out));
    EXPECT_TRUE(out.is_cover);
    EXPECT_EQ(out.position, 0.9f) << "the latest one";
}

TEST(ControlQueue, AReplacementNeedsNoRoomWhenTheLineIsFull) {
    ControlQueue q;
    ASSERT_TRUE(q.push(cover(0.1f)) == ControlQueue::Push::Queued);
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.push(sysop(30 + i)) == ControlQueue::Push::Queued);
    ASSERT_EQ(q.size(), ControlQueue::kDepth);
    EXPECT_TRUE(q.push(cover(0.7f)) == ControlQueue::Push::ReplacedCover);
    EXPECT_EQ(q.size(), ControlQueue::kDepth);
    EXPECT_TRUE(q.hasCover());
}

TEST(ControlQueue, SysopsAreNeverReplacedByEachOther) {
    ControlQueue q;
    ASSERT_TRUE(q.push(sysop(5)) == ControlQueue::Push::Queued);
    ASSERT_TRUE(q.push(sysop(5)) == ControlQueue::Push::Queued) << "the same sysop twice is two requests";
    EXPECT_EQ(q.size(), 2u);
}

TEST(ControlQueue, FindsAndRemovesAWaitingSysopWithoutDisturbingTheRest) {
    ControlQueue q;
    q.push(sysop(1));
    q.push(cover(0.3f));
    q.push(sysop(4));
    EXPECT_TRUE(q.hasSysop(4));
    EXPECT_FALSE(q.hasSysop(3));
    EXPECT_FALSE(q.removeSysop(3));
    EXPECT_EQ(q.size(), 3u);
    EXPECT_TRUE(q.removeSysop(4));
    EXPECT_FALSE(q.hasSysop(4));
    EXPECT_EQ(q.size(), 2u);
    EXPECT_TRUE(q.hasCover());
    ControlCmd out{};
    ASSERT_TRUE(q.pop(&out));
    EXPECT_EQ(out.sysop, 1);
    ASSERT_TRUE(q.pop(&out));
    EXPECT_TRUE(out.is_cover);
    q.push(sysop(9));
    q.clear();
    EXPECT_TRUE(q.empty());
}

TEST(ControlQueue, ACoverOpIsNotMistakenForASysopWithTheSameNumber) {
    ControlQueue q;
    q.push(ControlCmd{true, 1, 0, 0.0f, 4});     // a cover op whose stale sysop field reads 4
    EXPECT_FALSE(q.hasSysop(4));
    EXPECT_FALSE(q.removeSysop(4));
    EXPECT_TRUE(q.hasCover());
}

// The policy is pure and constexpr: usable with no listener at all.
static_assert(onAckTimeout(TimeoutState{false, false, false, 0, 4}).action == TimeoutAction::Stale, "");

// ---- Fire requests (step 6) ---------------------------------------------------------

TEST(FirePolicy, TheThreeUnackedFramesAreFireAndNeverRetryOrWaitOrGiveUp) {
    for (const Policy *p : {&kTimeSyncPolicy, &kConfigPushPolicy}) {
        EXPECT_EQ(p->mode, Mode::Fire);
        EXPECT_EQ(p->max_retries, 0) << "no retry ladder";
        EXPECT_EQ(p->base_wait_ms, 0u) << "no ack wait";
        EXPECT_FALSE(p->at_most_once);
        EXPECT_FALSE(p->supersedes_same_kind);
        EXPECT_FALSE(p->yields_warmup) << "a Fire frame leaves a warm-up running";
    }
}

TEST(FirePolicy, ATimeSyncIsAimedAtTheClassAWindowAndWaitsOutARelogin) {
    EXPECT_EQ(kTimeSyncPolicy.shape, Shape::ClassA);
    EXPECT_TRUE(kTimeSyncPolicy.defer_in_relogin);
}

TEST(FirePolicy, AConfigPushIsABareSendAndIsNotHeldBackByARelogin) {
    EXPECT_EQ(kConfigPushPolicy.shape, Shape::Bare);
    EXPECT_FALSE(kConfigPushPolicy.defer_in_relogin) << "the REGISTER that asks for it comes when no session exists";
}

TEST(FirePolicy, OnlyAFireRequestThatAsksForItIsDeferredAndOnlyDuringARelogin) {
    EXPECT_TRUE(fireDeferred(kTimeSyncPolicy, true));
    EXPECT_FALSE(fireDeferred(kTimeSyncPolicy, false)) << "no relogin, no deferral";
    EXPECT_FALSE(fireDeferred(kConfigPushPolicy, true)) << "it does not ask for it";
    EXPECT_FALSE(fireDeferred(kConfigPushPolicy, false));
}

TEST(FirePolicy, AnAckedPolicyIsNeverDeferredByTheFirePath) {
    // Their retries defer themselves (onAckTimeout); the Fire path must not also
    // swallow a first attempt even if a flag were set by mistake.
    for (const Policy *p : {&kSchedulePolicy, &kGridSyncPolicy, &kCoverPolicy, &kSysopPolicy,
                            &kSleepPolicy, &kOtaPolicy}) {
        EXPECT_EQ(p->mode, Mode::Acked);
        EXPECT_FALSE(p->defer_in_relogin);
        Policy flagged = *p;
        flagged.defer_in_relogin = true;
        EXPECT_FALSE(fireDeferred(flagged, true));
    }
}

static_assert(fireDeferred(kTimeSyncPolicy, true), "");
static_assert(!fireDeferred(kConfigPushPolicy, true), "");
