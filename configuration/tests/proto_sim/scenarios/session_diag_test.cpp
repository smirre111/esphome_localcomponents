// The hub's session diagnostics (SessionDiag.h): the stale-period predicate, the
// recovery-time measurement, the counters and the "Session State" text.
// Integration with the listener is pinned by SessionDiag in
// real_lora_client_test.cpp.

#include <gtest/gtest.h>

#include <cstring>

#include "SessionDiag.h"

using namespace sessiondiag;

// ---- the stale period -------------------------------------------------------

TEST(SessionDiagStale, ASilentNodeIsNeverStale) {
    Tracker t;
    t.noteLoginSent(0);
    t.noteLoginSent(5'000);
    EXPECT_FALSE(t.stale(10'000'000)) << "LOGINs into silence are not a stale period";
    EXPECT_EQ(t.msUntilStale(10'000'000), 0u);
    EXPECT_EQ(t.heardWithoutSessionMs(10'000'000), 0u);
}

TEST(SessionDiagStale, HeardWithoutASessionTurnsStaleAtTheThresholdAndNotBefore) {
    Tracker t;
    t.noteKeylessUplink(1'000);
    EXPECT_FALSE(t.stale(1'000 + kStaleAfterMs - 1));
    EXPECT_EQ(t.msUntilStale(1'000 + kStaleAfterMs - 1), 1u);
    EXPECT_TRUE(t.stale(1'000 + kStaleAfterMs));
    EXPECT_EQ(t.msUntilStale(1'000 + kStaleAfterMs), 0u) << "already stale: nothing to wait for";
}

TEST(SessionDiagStale, TheClockRunsFromTheFirstUplinkNotTheLatest) {
    Tracker t;
    t.noteKeylessUplink(0);
    t.noteKeylessUplink(20'000);   // must not restart the clock
    t.noteRegisterAccepted(25'000);
    EXPECT_TRUE(t.stale(kStaleAfterMs));
    EXPECT_EQ(t.heardWithoutSessionMs(kStaleAfterMs), kStaleAfterMs);
}

TEST(SessionDiagStale, AnAcceptedRegisterStartsTheClockToo) {
    Tracker t;
    t.noteRegisterAccepted(500);
    EXPECT_FALSE(t.stale(500 + kStaleAfterMs - 1));
    EXPECT_TRUE(t.stale(500 + kStaleAfterMs));
}

TEST(SessionDiagStale, ARefusedRegisterAloneDoesNotStartTheClock) {
    // It is counted; whether the node is stale is decided by uplinks the hub
    // attributes to a session-less node, which a refused frame is not proof of.
    Tracker t;
    t.noteRegisterRefused(RegisterRefusal::Replayed);
    EXPECT_FALSE(t.stale(10 * kStaleAfterMs));
    EXPECT_EQ(t.counters().registers_refused, 1u);
    EXPECT_TRUE(t.hasRefusal());
    EXPECT_EQ(t.lastRefusal(), RegisterRefusal::Replayed);
}

TEST(SessionDiagStale, AConfirmedSessionEndsItAndANewUplinkStartsAFreshClock) {
    Tracker t;
    t.noteKeylessUplink(0);
    ASSERT_TRUE(t.stale(kStaleAfterMs));
    t.noteConfirmed(kStaleAfterMs + 1);
    EXPECT_FALSE(t.stale(kStaleAfterMs + 2));
    t.noteKeylessUplink(100'000);
    EXPECT_FALSE(t.stale(100'000 + kStaleAfterMs - 1)) << "a fresh clock, not the old one";
    EXPECT_TRUE(t.stale(100'000 + kStaleAfterMs));
}

TEST(SessionDiagStale, SleepEndsIt) {
    Tracker t;
    t.noteKeylessUplink(0);
    ASSERT_TRUE(t.stale(kStaleAfterMs));
    t.noteAsleep();
    EXPECT_FALSE(t.stale(kStaleAfterMs + 1));
}

TEST(SessionDiagStale, PollCountsEachPeriodOnce) {
    Tracker t;
    bool became = true;
    t.noteKeylessUplink(0);
    EXPECT_FALSE(t.poll(kStaleAfterMs - 1, &became));
    EXPECT_FALSE(became);
    EXPECT_EQ(t.counters().stale_periods, 0u);

    EXPECT_TRUE(t.poll(kStaleAfterMs, &became));
    EXPECT_TRUE(became);
    EXPECT_TRUE(t.poll(kStaleAfterMs + 5'000, &became));
    EXPECT_FALSE(became) << "the same period";
    EXPECT_EQ(t.counters().stale_periods, 1u);

    t.noteConfirmed(50'000);
    t.noteKeylessUplink(60'000);
    EXPECT_TRUE(t.poll(60'000 + kStaleAfterMs, &became));
    EXPECT_TRUE(became);
    EXPECT_EQ(t.counters().stale_periods, 2u);
}

TEST(SessionDiagStale, AClockThatWentBackwardsReadsAsNoTimeElapsed) {
    Tracker t;
    t.noteKeylessUplink(10'000);
    EXPECT_FALSE(t.stale(5'000));
    EXPECT_EQ(t.heardWithoutSessionMs(5'000), 0u);
}

// ---- the recovery time ------------------------------------------------------

TEST(SessionDiagRecovery, RunsFromTheFirstLoginToTheConfirmationAndCountsItsLogins) {
    Tracker t;
    t.noteLoginSent(1'000);
    t.noteLoginSent(6'000);
    t.noteLoginSent(11'000);
    EXPECT_TRUE(t.recovering());
    EXPECT_EQ(t.recoveryLogins(), 3u);
    EXPECT_TRUE(t.noteConfirmed(12'500));
    EXPECT_FALSE(t.recovering());
    EXPECT_EQ(t.lastRecoveryMs(), 11'500u);
    EXPECT_EQ(t.lastRecoveryLogins(), 3u);
    EXPECT_EQ(t.counters().recoveries, 1u);
    EXPECT_EQ(t.recoveryLogins(), 0u) << "the next recovery counts its own";
}

TEST(SessionDiagRecovery, StartsAtAKeylessUplinkThatPrecedesTheLogin) {
    Tracker t;
    t.noteKeylessUplink(2'000);
    t.noteLoginSent(9'000);
    ASSERT_TRUE(t.noteConfirmed(10'000));
    EXPECT_EQ(t.lastRecoveryMs(), 8'000u);
    EXPECT_EQ(t.lastRecoveryLogins(), 1u);
}

TEST(SessionDiagRecovery, StartsAtADroppedSession) {
    Tracker t;
    t.noteSessionDropped(4'000);
    t.noteLoginSent(4'100);
    ASSERT_TRUE(t.noteConfirmed(7'000));
    EXPECT_EQ(t.lastRecoveryMs(), 3'000u);
    EXPECT_EQ(t.counters().sessions_dropped, 1u);
}

TEST(SessionDiagRecovery, AConfirmationWithNothingToRecoverMeasuresNothing) {
    Tracker t;
    EXPECT_FALSE(t.noteConfirmed(1'000));
    EXPECT_EQ(t.counters().recoveries, 0u);
    t.noteLoginSent(2'000);
    ASSERT_TRUE(t.noteConfirmed(3'000));
    EXPECT_FALSE(t.noteConfirmed(4'000)) << "every later uplink confirms again: not a recovery";
    EXPECT_EQ(t.counters().recoveries, 1u);
    EXPECT_EQ(t.lastRecoveryMs(), 1'000u);
}

TEST(SessionDiagRecovery, SleepCancelsTheMeasurement) {
    Tracker t;
    t.noteLoginSent(0);
    t.noteAsleep();
    EXPECT_FALSE(t.recovering());
    t.noteLoginSent(21'600'000);   // the wake login, six hours later
    ASSERT_TRUE(t.noteConfirmed(21'602'000));
    EXPECT_EQ(t.lastRecoveryMs(), 2'000u) << "not six hours";
    EXPECT_EQ(t.lastRecoveryLogins(), 1u);
}

// ---- counters ---------------------------------------------------------------

TEST(SessionDiagCounters, EachEventCountsOnce) {
    Tracker t;
    t.noteLoginSent(0);
    t.noteLoginSent(1);
    t.noteKeylessUplink(2);
    t.noteKeylessUplink(3);
    t.noteKeylessUplink(4);
    t.noteRegisterAccepted(5);
    t.noteRegisterRefused(RegisterRefusal::RateLimited);
    t.noteRegisterRefused(RegisterRefusal::BadTag);
    t.noteSessionDropped(6);
    const Counters &c = t.counters();
    EXPECT_EQ(c.logins_sent, 2u);
    EXPECT_EQ(c.keyless_uplinks, 3u);
    EXPECT_EQ(c.registers_accepted, 1u);
    EXPECT_EQ(c.registers_refused, 2u);
    EXPECT_EQ(c.sessions_dropped, 1u);
    EXPECT_EQ(t.lastRefusal(), RegisterRefusal::BadTag);
}

// ---- the state and its text -------------------------------------------------

TEST(SessionDiagState, AsleepBeatsConfirmedBeatsPending) {
    EXPECT_EQ(classify(true, true, true), State::Asleep);
    EXPECT_EQ(classify(false, true, true), State::Confirmed);
    EXPECT_EQ(classify(false, false, true), State::LoginPending);
    EXPECT_EQ(classify(false, false, false), State::NoSession);
}

TEST(SessionDiagState, StaleQualifiesOnlyTheSessionlessStates) {
    EXPECT_STREQ(stateText(State::Confirmed, false), "confirmed");
    EXPECT_STREQ(stateText(State::Confirmed, true), "confirmed");
    EXPECT_STREQ(stateText(State::Asleep, true), "asleep");
    EXPECT_STREQ(stateText(State::LoginPending, false), "login pending");
    EXPECT_STREQ(stateText(State::NoSession, false), "no session");
    EXPECT_STREQ(stateText(State::LoginPending, true), "stale (heard, no session)");
    EXPECT_STREQ(stateText(State::NoSession, true), "stale (heard, no session)");
}

TEST(SessionDiagState, RefusalTexts) {
    EXPECT_STREQ(refusalText(RegisterRefusal::BadTag), "bad tag");
    EXPECT_STREQ(refusalText(RegisterRefusal::Replayed), "replayed register_seq");
    EXPECT_STREQ(refusalText(RegisterRefusal::RateLimited), "rate-limited");
}

// ---- the event log ----------------------------------------------------------

TEST(SessionDiagEvents, KeepsEventsInOrderWithTheirTimes) {
    EventLog log;
    log.add(Event::RegisterAccepted, 7, 1'000);
    log.add(Event::LoginSent, 2, 5'000);
    log.add(Event::Confirmed, 4'500, 5'500);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log.at(0).kind, Event::RegisterAccepted);
    EXPECT_EQ(log.at(0).arg, 7u);
    EXPECT_EQ(log.at(1).kind, Event::LoginSent);
    EXPECT_EQ(log.at(1).first_ms, 5'000);
    EXPECT_EQ(log.at(2).kind, Event::Confirmed);
    EXPECT_EQ(log.at(2).arg, 4'500u);
}

TEST(SessionDiagEvents, ARunOfTheSameEventIsOneEntryWithACountAndItsSpan) {
    EventLog log;
    log.add(Event::LoginSent, 3, 0);
    for (int i = 1; i <= 9; ++i)
        log.add(Event::KeylessUplink, 0, i * 5'000);
    log.add(Event::LoginSent, 1, 60'000);
    ASSERT_EQ(log.size(), 3u) << "a node transmitting every 5 s must not push the history out";
    EXPECT_EQ(log.at(1).count, 9u);
    EXPECT_EQ(log.at(1).first_ms, 5'000);
    EXPECT_EQ(log.at(1).last_ms, 45'000);
    EXPECT_EQ(log.at(2).count, 1u) << "the same kind again after another is a new entry";
    EXPECT_EQ(log.at(2).arg, 1u);
}

TEST(SessionDiagEvents, ARunKeepsTheArgumentOfItsLastEvent) {
    EventLog log;
    log.add(Event::LoginSent, 3, 0);
    log.add(Event::LoginSent, 0, 5'000);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log.at(0).arg, 0u);
    EXPECT_EQ(log.at(0).count, 2u);
}

TEST(SessionDiagEvents, WhenFullTheOldestEntryGoes) {
    EventLog log;
    // Alternate two kinds so nothing coalesces.
    for (uint32_t i = 0; i < kEventLogSize + 5; ++i)
        log.add((i % 2) ? Event::LoginSent : Event::KeylessUplink, i, (int64_t) i * 1000);
    ASSERT_EQ(log.size(), kEventLogSize);
    EXPECT_EQ(log.at(0).arg, 5u) << "entries 0..4 were dropped";
    EXPECT_EQ(log.at(kEventLogSize - 1).arg, kEventLogSize + 4);
    for (uint32_t i = 1; i < log.size(); ++i)
        EXPECT_EQ(log.at(i).arg, log.at(i - 1).arg + 1) << "still in order across the wrap";
}

TEST(SessionDiagEvents, EveryEventHasAText) {
    for (int e = 0; e <= (int) Event::NodeReport; ++e)
        EXPECT_GT(std::strlen(eventText((Event) e)), 3u) << e;
    EXPECT_STREQ(eventText(Event::LoginSent), "LOGIN sent");
    EXPECT_STREQ(eventText(Event::StaleRaised), "STALE raised");
    EXPECT_STREQ(eventText(Event::NodeRestored), "node restored its counters");
}

// ---- the node's own account (beacon) ----------------------------------------

TEST(SessionDiagNodeReport, AnEmptyReportIsNeverNews) {
    NodeReport none, last;
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(nodeReportIsNews(none, last));
    last.registers_sent = 3;
    EXPECT_FALSE(nodeReportIsNews(none, last)) << "the node rebooted: zeros say nothing";
}

TEST(SessionDiagNodeReport, ARepeatIsNotNewsAndAnyChangedFieldIs) {
    NodeReport r;
    r.registers_sent = 2; r.login_wait_ms = 6100; r.login_copy = 5;
    NodeReport last;
    EXPECT_TRUE(nodeReportIsNews(r, last));
    last = r;
    EXPECT_FALSE(nodeReportIsNews(r, last)) << "every beacon repeats it";
    NodeReport a = r; a.registers_sent = 3;
    NodeReport b = r; b.login_wait_ms = 6101;
    NodeReport c = r; c.login_copy = 6;
    NodeReport d = r; d.regseq_save_fails = 1;
    EXPECT_TRUE(nodeReportIsNews(a, last));
    EXPECT_TRUE(nodeReportIsNews(b, last));
    EXPECT_TRUE(nodeReportIsNews(c, last));
    EXPECT_TRUE(nodeReportIsNews(d, last));
}

TEST(SessionDiagNodeReport, ASaveFailureAloneIsAReport) {
    NodeReport r;
    r.regseq_save_fails = 1;
    EXPECT_FALSE(r.empty());
    EXPECT_TRUE(nodeReportIsNews(r, NodeReport{}));
}
