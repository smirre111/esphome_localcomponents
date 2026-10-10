// ModeBSupervisor.h: the policy that keeps a node whose Timed Mode switch is ON
// actually in Mode B across node sleep, node reboot and hub reboot.
//
// Pure policy, no listener: the glue (what a warm-up physically is, where the
// inputs come from) is tested in real_lora_client_test.cpp. These tests drive the
// state machine with hand-built Inputs and a virtual uptime-seconds clock.

#include "ModeBSupervisor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace modebsup;

namespace {

// A node that is logged in, awake, on a settled and acknowledged grid.
Inputs ready() {
    Inputs in;
    in.supervised       = true;
    in.session_ok       = true;
    in.awake            = true;
    in.s_since_grid_ack = kGridSettleS + 1;
    return in;
}

// A deterministic stand-in for esp_random().
struct Rng {
    uint32_t s{12345};
    uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

// Run ticks (1 per kTickS) until `want` is returned or `limit_s` passes.
// Returns the time of the event, or UINT32_MAX.
uint32_t runUntil(Supervisor &sv, Arbiter &arb, Inputs in, uint32_t id, uint32_t &now,
                  Event want, uint32_t limit_s, Rng &rng) {
    const uint32_t end = now + limit_s;
    while (!reached(now, end)) {
        if (sv.tick(in, now, rng.next(), arb, id) == want)
            return now;
        now += kTickS;
    }
    return UINT32_MAX;
}

}  // namespace

// --- constants ---------------------------------------------------------------

TEST(ModeBBackoff, IsTwoFiveFifteenMinutesThenHourly) {
    EXPECT_EQ(backoffS(0), 0u);
    EXPECT_EQ(backoffS(1), 120u);
    EXPECT_EQ(backoffS(2), 300u);
    EXPECT_EQ(backoffS(3), 900u);
    EXPECT_EQ(backoffS(4), 3600u);
    EXPECT_EQ(backoffS(5), 3600u);
    EXPECT_EQ(backoffS(1000), 3600u) << "capped: never faster than hourly after four failures";
}

TEST(ModeBConstants, TheTokenLeaseOutlivesAWarmupAndTheHubsStopTimer) {
    // start_mode_test stops itself at duration + 5 s.
    EXPECT_GT(kLeaseS, kWarmupS + 5);
    // The node announces a mode change at most once per 60 s (TimedModePolicy.h
    // kModeAnnounceMinS); the verify window must cover one full floor.
    EXPECT_GE(kVerifyGraceS, 60u);
    // Runbook: a warm-up 46 s after grid adoption measured 0/0, 73 s measured 193/192.
    EXPECT_GT(kGridSettleS, 73u);
}

TEST(ModeBConstants, TheWorstCaseForThirtyTwoNodesIsUnderFortyMinutes) {
    EXPECT_LE(worstCaseAllNodesS(32), 40u * 60u);
    EXPECT_EQ(worstCaseAllNodesS(0), kGridSettleS + kJitterMaxS);
}

TEST(ModeBConstants, ReachedSurvivesUptimeWrap) {
    EXPECT_TRUE(reached(10, 10));
    EXPECT_FALSE(reached(9, 10));
    EXPECT_TRUE(reached(5, 0xFFFFFFF0u)) << "5 s after the wrap is past 16 s before it";
    EXPECT_FALSE(reached(0xFFFFFFF0u, 5));
}

TEST(ModeBReport, OnlyRealModeBWindowsCount) {
    EXPECT_TRUE(reportShowsModeB(2, 10, 10));
    EXPECT_FALSE(reportShowsModeB(1, 10, 10)) << "a Mode A run";
    EXPECT_FALSE(reportShowsModeB(2, 0, 0)) << "mode=2 windows 0/0 is timed RX enabled but never active";
    EXPECT_FALSE(reportShowsModeB(2, 10, 0)) << "armed but nothing heard";
}

// --- the gate ladder ---------------------------------------------------------

TEST(ModeBGate, EachReasonIsNamedInOrder) {
    Inputs in = ready();
    EXPECT_EQ(commonGate(in), Gate::Ok);
    EXPECT_EQ(startGate(in), Gate::Ok);

    in = ready(); in.supervised = false;
    EXPECT_EQ(commonGate(in), Gate::NotSupervised);
    in = ready(); in.session_ok = false;
    EXPECT_EQ(commonGate(in), Gate::NoSession);
    in = ready(); in.awake = false;
    EXPECT_EQ(commonGate(in), Gate::Asleep);

    in = ready(); in.s_since_grid_ack = 0xFFFFFFFFu;
    EXPECT_EQ(startGate(in), Gate::GridNotAcked);
    in = ready(); in.s_since_grid_ack = kGridSettleS - 1;
    EXPECT_EQ(startGate(in), Gate::GridSettling);
    in = ready(); in.s_since_grid_ack = kGridSettleS;
    EXPECT_EQ(startGate(in), Gate::Ok) << "settled exactly at the bound";
    in = ready(); in.promoted = true;
    EXPECT_EQ(startGate(in), Gate::Promoted);
    in = ready(); in.busy = true;
    EXPECT_EQ(startGate(in), Gate::Busy);
    in = ready(); in.other_test = true;
    EXPECT_EQ(startGate(in), Gate::OtherTest);
}

// --- the hub-wide token ------------------------------------------------------

TEST(ModeBArbiter, OnlyTheHeadOfTheQueueIsGrantedAndOnlyOneAtATime) {
    Arbiter a;
    ASSERT_TRUE(a.request(7));
    ASSERT_TRUE(a.request(8));
    ASSERT_TRUE(a.request(9));
    EXPECT_FALSE(a.granted(8, 0)) << "8 is not the head";
    EXPECT_TRUE(a.granted(7, 0));
    EXPECT_FALSE(a.granted(8, 0)) << "7 holds it";
    EXPECT_FALSE(a.granted(9, 0));
    a.cancel(7);
    EXPECT_FALSE(a.granted(9, 0)) << "FIFO: 8 asked before 9";
    EXPECT_TRUE(a.granted(8, 0));
    a.cancel(8);
    EXPECT_TRUE(a.granted(9, 0));
}

TEST(ModeBArbiter, RequestingTwiceDoesNotQueueTwice) {
    Arbiter a;
    EXPECT_TRUE(a.request(1));
    EXPECT_TRUE(a.request(1));
    EXPECT_EQ(a.waiting(), 1u);
    EXPECT_TRUE(a.granted(1, 0));
    EXPECT_TRUE(a.request(1)) << "the holder asking again is not a new entry";
    EXPECT_EQ(a.waiting(), 0u);
}

TEST(ModeBArbiter, ALeaseThatIsNeverReleasedCannotStallTheFleet) {
    Arbiter a;
    a.request(1);
    a.request(2);
    ASSERT_TRUE(a.granted(1, 100));
    EXPECT_FALSE(a.granted(2, 100 + kLeaseS - 1));
    EXPECT_FALSE(a.holds(2, 100 + kLeaseS - 1));
    EXPECT_TRUE(a.granted(2, 100 + kLeaseS)) << "holder 1 never released: the lease frees it";
    EXPECT_FALSE(a.holds(1, 100 + kLeaseS)) << "and 1 learns it lost the token";
}

TEST(ModeBArbiter, AnOpHoldsOffTheNextGrantButNotAHolder) {
    Arbiter a;
    a.request(1);
    a.request(2);
    ASSERT_TRUE(a.granted(1, 50));
    a.holdOff(60, kOpHoldOffS);
    EXPECT_TRUE(a.holds(1, 61)) << "the hold-off never revokes a running warm-up";
    a.cancel(1);
    EXPECT_FALSE(a.granted(2, 60 + kOpHoldOffS - 1));
    EXPECT_TRUE(a.granted(2, 60 + kOpHoldOffS));
}

TEST(ModeBArbiter, ALongerHoldOffIsNotShortenedByAShorterOne) {
    Arbiter a;
    a.holdOff(0, 30);
    a.holdOff(0, 5);
    a.request(1);
    EXPECT_FALSE(a.granted(1, 29));
    EXPECT_TRUE(a.granted(1, 30));
}

TEST(ModeBArbiter, ACancelledWaiterLeavesTheQueue) {
    Arbiter a;
    a.request(1); a.request(2); a.request(3);
    a.cancel(2);
    EXPECT_EQ(a.waiting(), 2u);
    EXPECT_TRUE(a.granted(1, 0));
    a.cancel(1);
    EXPECT_TRUE(a.granted(3, 0));
}

TEST(ModeBArbiter, ACapacityOverflowIsRefusedNotCorrupting) {
    Arbiter a;
    for (uint32_t i = 0; i < Arbiter::kCap; ++i)
        ASSERT_TRUE(a.request(i));
    EXPECT_FALSE(a.request(1000));
    EXPECT_TRUE(a.request(5)) << "an existing entry is still idempotent";
    EXPECT_EQ(a.waiting(), Arbiter::kCap);
}

// --- one node ----------------------------------------------------------------

// SWITCH ON -> the node is logged in -> exactly one warm-up starts, after the
// settle and the jitter, never before.
TEST(ModeBSupervisor, ALoggedInNodeIsWarmedUpOnceItsGridHasSettled) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.s_since_grid_ack = 0;           // GridSync just acknowledged

    uint32_t started = UINT32_MAX;
    uint32_t starts = 0;
    for (uint32_t t = 0; t < 300; t += kTickS, now += kTickS) {
        in.s_since_grid_ack = t;
        if (started != UINT32_MAX && t >= started + kWarmupS + 5)
            in.warmup_running = false;    // the hub's stop timer fired
        const Event ev = sv.tick(in, now, rng.next(), arb, 1);
        if (ev == Event::Start) {
            ++starts;
            if (started == UINT32_MAX) started = t;
            in.warmup_running = true;     // the glue starts the ModeTest
        }
    }
    EXPECT_EQ(starts, 1u) << "one Start per attempt, however many ticks follow";
    EXPECT_GE(started, kGridSettleS) << "never into an unsettled grid";
    EXPECT_LE(started, kGridSettleS + kJitterMaxS + 2 * kTickS);
    EXPECT_EQ(sv.failures(), 1u) << "no promotion within the verify window: one failure, not a re-start loop";
    EXPECT_EQ(sv.phase(), Phase::Idle) << "and now it waits out its 2-minute backoff";
}

TEST(ModeBSupervisor, ANodeThatIsAlreadyPromotedIsNeverWarmedUp) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.promoted = true;
    for (int i = 0; i < 1000; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    EXPECT_EQ(sv.phase(), Phase::Idle);
    EXPECT_EQ(arb.waiting(), 0u) << "it never even asked for the token";
    EXPECT_FALSE(arb.held());
}

// MODE A / AUTO-MODE / switch OFF: untouched, nothing requested, ever.
TEST(ModeBSupervisor, ANodeWhoseTimedModeIsOffIsNeverTouched) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.supervised = false;
    for (int i = 0; i < 1000; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    EXPECT_EQ(sv.phase(), Phase::Idle);
    EXPECT_EQ(arb.waiting(), 0u);
    EXPECT_FALSE(arb.held());
    EXPECT_EQ(sv.failures(), 0u);
}

TEST(ModeBSupervisor, NothingStartsWithoutASessionOrWhileTheNodeSleeps) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.session_ok = false;
    for (int i = 0; i < 200; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    in.session_ok = true; in.awake = false;
    for (int i = 0; i < 200; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    EXPECT_EQ(arb.waiting(), 0u);
}

TEST(ModeBSupervisor, AnUnacknowledgedGridIsNeverWarmedUp) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.s_since_grid_ack = 0xFFFFFFFFu;
    for (int i = 0; i < 500; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    EXPECT_EQ(arb.waiting(), 0u);
}

// A WARM-UP THAT WORKED
TEST(ModeBSupervisor, APromotionAfterTheWarmupIsRecordedAndHeldAgainstFlapping) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    ASSERT_EQ(sv.phase(), Phase::Warming);

    in.warmup_running = true;
    EXPECT_EQ(sv.tick(in, now + 10, 0, arb, 1), Event::None);
    in.warmup_running = false;                          // the hub's stop timer fired
    EXPECT_EQ(sv.tick(in, now + 65, 0, arb, 1), Event::None);
    EXPECT_EQ(sv.phase(), Phase::Verifying);
    EXPECT_FALSE(arb.held()) << "the air is free for the next node as soon as the marks stop";

    in.promoted = true;                                 // the node's mode-change announcement
    EXPECT_EQ(sv.tick(in, now + 80, 0, arb, 1), Event::Succeeded);
    EXPECT_EQ(sv.failures(), 0u);
    EXPECT_EQ(sv.phase(), Phase::Idle);

    // Demoted again right away: no new warm-up inside the flap guard.
    in.promoted = false;
    uint32_t t = now + 82;
    for (; t < now + 80 + kRearmHoldS - 1; t += kTickS)
        ASSERT_EQ(sv.tick(in, t, 0, arb, 1), Event::None) << "t=" << t - now;
}

TEST(ModeBSupervisor, TheModeTestReportAloneAlsoCountsAsSuccess) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    in.report_ok = true;
    EXPECT_EQ(sv.tick(in, now + 66, 0, arb, 1), Event::Succeeded);
}

// A WARM-UP THAT FAILED -> backoff 2, 5, 15 min, then hourly
TEST(ModeBSupervisor, FailedWarmupsBackOffTwoFiveFifteenThenHourly) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    const uint32_t expect[] = {120, 300, 900, 3600, 3600, 3600};
    uint32_t failed_at = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        // the node stays logged in, awake, settled and NOT promoted
        in.warmup_running = false;
        const uint32_t start = runUntil(sv, arb, in, 1, now, Event::Start, 20000, rng);
        ASSERT_NE(start, UINT32_MAX) << "attempt " << attempt;
        if (attempt > 0) {
            // not before the previous failure + its backoff
            EXPECT_GE(start - failed_at, expect[attempt - 1])
                << "attempt " << attempt << " started too early";
            EXPECT_LE(start - failed_at, expect[attempt - 1] + kJitterMaxS + 2 * kTickS);
        }
        in.warmup_running = true;
        now += 65;                                   // the marks run
        in.warmup_running = false;
        sv.tick(in, now, 0, arb, 1);                 // -> Verifying
        ASSERT_EQ(sv.phase(), Phase::Verifying);
        const uint32_t v0 = now;
        Event ev = Event::None;
        while (ev == Event::None && now < v0 + 1000) { now += kTickS; ev = sv.tick(in, now, 0, arb, 1); }
        ASSERT_EQ(ev, Event::Failed);
        EXPECT_GE(now - v0, kVerifyGraceS);
        failed_at = now;
        EXPECT_EQ(sv.failures(), static_cast<uint32_t>(attempt + 1));
    }
}

// NODE REBOOT / RELOGIN / WAKE -> warm-up again with the backoff reset
TEST(ModeBSupervisor, ARestartForgetsTheBackoffAndWarmsUpAgainAtOnce) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now, 0, arb, 1);
    Event ev = Event::None;
    while (ev == Event::None) { now += kTickS; ev = sv.tick(in, now, 0, arb, 1); }
    ASSERT_EQ(ev, Event::Failed);
    ASSERT_EQ(sv.failures(), 1u);
    ASSERT_GE(sv.notBefore(), now + 120);

    sv.restart(arb, 1);                      // the node logged in again
    EXPECT_EQ(sv.failures(), 0u);
    const uint32_t restart_at = now;
    const uint32_t start = runUntil(sv, arb, in, 1, now, Event::Start, 200, rng);
    ASSERT_NE(start, UINT32_MAX);
    EXPECT_LE(start - restart_at, kJitterMaxS + 2 * kTickS)
        << "a restart drops the 2-minute backoff: only the jitter remains";
}

TEST(ModeBSupervisor, ALoginTearsDownAWarmupInProgressWithoutLeakingTheToken) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    ASSERT_TRUE(arb.held());
    sv.restart(arb, 1);
    EXPECT_FALSE(arb.held());
    EXPECT_EQ(sv.phase(), Phase::Idle);
}

// THE NODE FELL ASLEEP / LOST ITS SESSION MID-WARM-UP
TEST(ModeBSupervisor, ANodeThatSleepsMidWarmupIsAbandonedAndReleasesTheToken) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = true;
    in.awake = false;
    EXPECT_EQ(sv.tick(in, now + 5, 0, arb, 1), Event::DropOut);
    EXPECT_FALSE(arb.held());
    EXPECT_EQ(sv.failures(), 0u) << "the node leaving is not a failed warm-up";
}

TEST(ModeBSupervisor, ALostLeaseStopsTheWarmupSoTwoNeverOverlap) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = true;
    EXPECT_EQ(sv.tick(in, now + kLeaseS, 0, arb, 1), Event::DropOut);
}

// COVER OP PENDING -> the warm-up yields
TEST(ModeBSupervisor, ABusyNodeDoesNotStartAndTriesAgainAfterThirtySeconds) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.busy = true;
    for (int i = 0; i < 100; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None) << "busy: no token request";
    EXPECT_EQ(arb.waiting(), 0u);
    in.busy = false;
    EXPECT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 60, rng), UINT32_MAX);
}

TEST(ModeBSupervisor, AnOpIssuedWhileQueuedGivesWayWithoutAFailure) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    arb.request(99);
    ASSERT_TRUE(arb.granted(99, now));         // somebody else is warming up
    Inputs in = ready();
    // become due and queue behind 99
    for (int i = 0; i < 100 && sv.phase() != Phase::Queued; ++i, now += kTickS)
        sv.tick(in, now, rng.next(), arb, 1);
    ASSERT_EQ(sv.phase(), Phase::Queued);
    in.busy = true;                            // a cover op for THIS node
    EXPECT_EQ(sv.tick(in, now, 0, arb, 1), Event::Yield);
    EXPECT_EQ(sv.phase(), Phase::Idle);
    EXPECT_EQ(arb.waiting(), 0u);
    EXPECT_EQ(sv.failures(), 0u);
    EXPECT_GE(sv.notBefore(), now + kYieldRetryS);
}

TEST(ModeBSupervisor, AnOpIssuedDuringTheWarmupAbortsItWithoutAFailure) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    ASSERT_TRUE(sv.yieldToOp(arb, 1, now + 3));
    EXPECT_EQ(sv.phase(), Phase::Idle);
    EXPECT_FALSE(arb.held());
    EXPECT_EQ(sv.failures(), 0u);
    EXPECT_GE(sv.notBefore(), now + 3 + kYieldRetryS);
    // and not a second time: nothing is running any more
    EXPECT_FALSE(sv.yieldToOp(arb, 1, now + 4));
}

TEST(ModeBSupervisor, AnOpWhileOnlyVerifyingLeavesTheVerificationAlone) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    ASSERT_EQ(sv.phase(), Phase::Verifying);
    EXPECT_FALSE(sv.yieldToOp(arb, 1, now + 66));
    EXPECT_EQ(sv.phase(), Phase::Verifying);
}

TEST(ModeBSupervisor, AManualTestOnTheNodeBlocksTheStartButIsNotCounted) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    in.other_test = true;
    for (int i = 0; i < 100; ++i, now += kTickS)
        ASSERT_EQ(sv.tick(in, now, rng.next(), arb, 1), Event::None);
    EXPECT_EQ(arb.waiting(), 0u);
    EXPECT_EQ(sv.failures(), 0u);
}

TEST(ModeBSupervisor, ASuccessAfterAFailureClearsTheCountSoTheNextBackoffStartsAtTwoMinutes) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    // attempt 1 fails ...
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    Event ev = Event::None;
    uint32_t t = now + 66;
    for (; ev != Event::Failed && t < now + 400; t += kTickS)
        ev = sv.tick(in, t, 0, arb, 1);
    ASSERT_EQ(ev, Event::Failed);
    ASSERT_EQ(sv.failures(), 1u);
    // ... attempt 2 succeeds ...
    now = t + 200;
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 400, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    in.promoted = true;
    ASSERT_EQ(sv.tick(in, now + 70, 0, arb, 1), Event::Succeeded);
    EXPECT_EQ(sv.failures(), 0u) << "success wipes the history";
    // ... and when it is demoted again the first failure costs 2 minutes, not 5.
    in.promoted = false;
    now += 70 + kRearmHoldS + 10;
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    ev = Event::None;
    t = now + 66;
    for (; ev != Event::Failed && t < now + 400; t += kTickS)
        ev = sv.tick(in, t, 0, arb, 1);
    ASSERT_EQ(ev, Event::Failed);
    EXPECT_EQ(sv.failures(), 1u);
    EXPECT_LT(sv.notBefore() - (t - kTickS), 121u) << "backoff(1) = 120 s";
}

TEST(ModeBSupervisor, APromotionSeenWhileIdleClearsTheFailureCount) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    Inputs in = ready();
    ASSERT_NE(runUntil(sv, arb, in, 1, now, Event::Start, 200, rng), UINT32_MAX);
    in.warmup_running = false;
    sv.tick(in, now + 65, 0, arb, 1);
    Event ev = Event::None;
    for (uint32_t t = now + 66; ev != Event::Failed && t < now + 400; t += kTickS)
        ev = sv.tick(in, t, 0, arb, 1);
    ASSERT_EQ(ev, Event::Failed);
    ASSERT_EQ(sv.failures(), 1u);
    in.promoted = true;                       // the node got there by itself after all
    sv.tick(in, now + 500, 0, arb, 1);
    EXPECT_EQ(sv.failures(), 0u) << "a node in Mode B is not a failing node";
}

TEST(ModeBSupervisor, AManualTestAppearingWhileQueuedGivesWayWithoutAFailure) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    arb.request(99);
    ASSERT_TRUE(arb.granted(99, now));
    Inputs in = ready();
    for (int i = 0; i < 100 && sv.phase() != Phase::Queued; ++i, now += kTickS)
        sv.tick(in, now, rng.next(), arb, 1);
    ASSERT_EQ(sv.phase(), Phase::Queued);
    in.other_test = true;
    EXPECT_EQ(sv.tick(in, now, 0, arb, 1), Event::Yield);
    EXPECT_EQ(sv.failures(), 0u);
    EXPECT_EQ(arb.waiting(), 0u);
}

TEST(ModeBSupervisor, ALostGridAckWhileQueuedSendsTheNodeBackToSettling) {
    Supervisor sv; Arbiter arb; Rng rng; uint32_t now = 1000;
    arb.request(99);
    ASSERT_TRUE(arb.granted(99, now));
    Inputs in = ready();
    for (int i = 0; i < 100 && sv.phase() != Phase::Queued; ++i, now += kTickS)
        sv.tick(in, now, rng.next(), arb, 1);
    ASSERT_EQ(sv.phase(), Phase::Queued);
    in.s_since_grid_ack = 3;                  // the grid was just re-published and re-acked
    EXPECT_EQ(sv.tick(in, now, 0, arb, 1), Event::None);
    EXPECT_EQ(sv.phase(), Phase::Idle);
    EXPECT_EQ(arb.waiting(), 0u);
    EXPECT_EQ(sv.failures(), 0u);
}

// --- the fleet -----------------------------------------------------------------

struct Fleet {
    struct Node {
        Supervisor sv;
        Inputs     in;
        uint32_t   run_until{0};      // warm-up on the air until
        uint32_t   promote_at{0};
        uint32_t   started_at{0};
        uint32_t   starts{0};
        bool       running{false};
    };
    std::vector<Node> n;
    Arbiter arb;
    Rng rng;
    uint32_t max_concurrent{0};
    uint32_t now{1000};

    explicit Fleet(size_t count) : n(count) {
        for (auto &x : n) x.in = ready();
    }

    // One tick of every node, in an order that changes tick to tick.
    void step() {
        uint32_t active = 0;
        for (size_t k = 0; k < n.size(); ++k) {
            const size_t i = (k + now / kTickS) % n.size();
            Node &x = n[i];
            if (x.running && reached(now, x.run_until)) { x.running = false; x.promote_at = now + 20; }
            if (x.promote_at != 0 && reached(now, x.promote_at)) x.in.promoted = true;
            x.in.warmup_running = x.running;
            const Event ev = x.sv.tick(x.in, now, rng.next(), arb, static_cast<uint32_t>(i));
            if (ev == Event::Start) {
                x.running = true;
                x.run_until = now + kWarmupS + 5;     // start_mode_test's own stop timer
                x.started_at = now;
                ++x.starts;
            }
            if (x.running) ++active;
        }
        max_concurrent = std::max(max_concurrent, active);
        now += kTickS;
    }
};

// 32 NODES WAKING TOGETHER
TEST(ModeBFleet, ThirtyTwoNodesThatWakeTogetherAreSerialisedAndAllRecover) {
    Fleet f(32);
    const uint32_t t0 = f.now;
    uint32_t horizon = worstCaseAllNodesS(32) + 60;
    while (!reached(f.now, t0 + horizon)) f.step();

    EXPECT_LE(f.max_concurrent, 1u) << "never two warm-ups on the air at once";
    uint32_t recovered = 0;
    uint32_t last_start = 0;
    for (auto &x : f.n) {
        EXPECT_EQ(x.starts, 1u) << "each node warmed up exactly once";
        recovered += x.in.promoted ? 1 : 0;
        last_start = std::max(last_start, x.started_at);
    }
    EXPECT_EQ(recovered, 32u);
    EXPECT_LE(last_start - t0, worstCaseAllNodesS(32)) << "inside the documented worst case";
}

TEST(ModeBFleet, StartsAreSpacedByAtLeastAWarmupAndAreFirstComeFirstServed) {
    Fleet f(8);
    const uint32_t t0 = f.now;
    while (!reached(f.now, t0 + worstCaseAllNodesS(8) + 60)) f.step();
    std::vector<uint32_t> starts;
    for (auto &x : f.n) { ASSERT_EQ(x.starts, 1u); starts.push_back(x.started_at); }
    std::sort(starts.begin(), starts.end());
    for (size_t i = 1; i < starts.size(); ++i)
        EXPECT_GE(starts[i] - starts[i - 1], kWarmupS)
            << "the next warm-up only starts after the previous marks stopped";
}

TEST(ModeBFleet, TheJitterSpreadsTheFirstRequestsSoTheyDoNotAllArriveInOneTick) {
    // Not the correctness guarantee (the FIFO is) - the thing that stops 32
    // nodes asking in the same tick.
    Fleet f(32);
    std::vector<uint32_t> first_queued(32, 0);
    const uint32_t t0 = f.now;
    for (uint32_t k = 0; k < 40; ++k) {
        f.step();
        for (size_t i = 0; i < 32; ++i)
            if (first_queued[i] == 0 && f.n[i].sv.phase() != Phase::Idle)
                first_queued[i] = f.now - t0;
    }
    std::sort(first_queued.begin(), first_queued.end());
    first_queued.erase(std::unique(first_queued.begin(), first_queued.end()), first_queued.end());
    EXPECT_GE(first_queued.size(), 5u) << "requests are spread over several ticks";
}

TEST(ModeBFleet, AFailingNodeDoesNotBlockTheOthers) {
    Fleet f(4);
    // node 0 is never promoted by its warm-up
    auto step_and_pin = [&]() { f.step(); f.n[0].in.promoted = false; f.n[0].promote_at = 0; };
    const uint32_t t0 = f.now;
    while (!reached(f.now, t0 + worstCaseAllNodesS(4) + 400)) step_and_pin();
    for (size_t i = 1; i < 4; ++i) EXPECT_TRUE(f.n[i].in.promoted) << "node " << i;
    EXPECT_GE(f.n[0].sv.failures(), 1u);
    EXPECT_LE(f.max_concurrent, 1u);
}

// A HUB REBOOT: every node is demoted by the startup broadcast, the Timed Mode
// switch restored ON, all 32 nodes log in again within a minute or two.
TEST(ModeBFleet, AHubRebootThatDemotesEveryNodeBringsEveryNodeBack) {
    Fleet f(32);
    for (auto &x : f.n) x.in.promoted = true;     // they were all in Mode B
    const uint32_t t0 = f.now;
    for (int i = 0; i < 100; ++i) f.step();
    for (auto &x : f.n) ASSERT_EQ(x.starts, 0u) << "promoted nodes are left alone";

    // the reboot: nobody promoted, sessions are being re-built (login stagger)
    for (size_t i = 0; i < f.n.size(); ++i) {
        f.n[i].in.promoted = false;
        f.n[i].in.session_ok = false;
        f.n[i].in.s_since_grid_ack = 0xFFFFFFFFu;
        f.n[i].sv.restart(f.arb, static_cast<uint32_t>(i));
    }
    const uint32_t reboot = f.now;
    while (!reached(f.now, reboot + 4000)) {
        const uint32_t since = f.now - reboot;
        for (size_t i = 0; i < f.n.size(); ++i) {
            // node i logs in at 3 s * i, its GridSync is acked 2 s later
            if (since >= 3 * i)     f.n[i].in.session_ok = true;
            if (since >= 3 * i + 2) f.n[i].in.s_since_grid_ack = since - (3 * i + 2);
        }
        f.step();
    }
    (void) t0;
    for (size_t i = 0; i < f.n.size(); ++i)
        EXPECT_TRUE(f.n[i].in.promoted) << "node " << i << " was not re-established";
    EXPECT_LE(f.max_concurrent, 1u);
}
