// AutoModePolicy — the arithmetic automatic mode is made of.
//
// Every one of these behaviours has been wrong in production at least once, and
// every one was found on hardware because the decision was buried in a
// 2,900-line class that needed a motor, a radio and two mutexes to construct.
// Here they are scalars.

#include <gtest/gtest.h>

#include "AutoModePolicy.h"

using namespace automode;

namespace {
constexpr uint64_t kNoon = 1787054400ULL;   // an arbitrary fixed epoch
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

TEST(AutoModePolicy, RunsOnlyWithEverythingItNeeds) {
    EXPECT_TRUE(shouldRun(/*auto=*/true, /*tempInteractive=*/false,
                          /*clock=*/true, /*schedule=*/true));
}

TEST(AutoModePolicy, RefusesWithoutAClock) {
    // I8: a node that cannot evaluate its schedule must never sleep against it —
    // it would compute nonsense wake times and could disappear for years.
    EXPECT_EQ(refusalFor(true, false, /*clock=*/false, true), Refusal::NoClock);
    EXPECT_FALSE(shouldRun(true, false, false, true));
}

TEST(AutoModePolicy, RefusesWithoutAUsableSchedule) {
    // Q9's runtime half: sleeping towards nothing strands the node until its
    // check-in, or forever if that is disabled too.
    EXPECT_EQ(refusalFor(true, false, true, /*schedule=*/false),
              Refusal::NoUsableSchedule);
}

TEST(AutoModePolicy, AButtonOverrideSuspendsIt) {
    EXPECT_EQ(refusalFor(true, /*tempInteractive=*/true, true, true),
              Refusal::TemporarilyInteractive);
}

TEST(AutoModePolicy, RefusalOrderPutsTheActionableReasonFirst) {
    // The caller reacts differently per reason — a missing clock is worth
    // asking for, a missing schedule is not — so the order is load-bearing.
    // Mode off must win over everything: a node the hub has put in interactive
    // mode should not be arming clock retries.
    EXPECT_EQ(refusalFor(/*auto=*/false, true, false, false), Refusal::ModeOff);
}

// ---------------------------------------------------------------------------
// Lookback — F19
// ---------------------------------------------------------------------------

TEST(AutoModePolicy, CatchupZeroStillSearchesTheGraceWindow) {
    // catchup_window 0 means "never replay a MISS", not "never run". Returning
    // the raw value disabled automatic mode outright: the node woke on time,
    // beaconed, and never moved the blind — observed on node 1 for two events.
    EXPECT_EQ(lookbackSeconds(0), kDueGraceS);
    EXPECT_GT(lookbackSeconds(0), 0u);
}

TEST(AutoModePolicy, ALargerCatchupWindowWins) {
    EXPECT_EQ(lookbackSeconds(1800), 1800u);
}

TEST(AutoModePolicy, ASmallCatchupWindowIsRaisedToTheGrace) {
    // Anything below the grace would let an entry that fell due seconds ago be
    // missed, which is the normal path, not an edge case.
    EXPECT_EQ(lookbackSeconds(30), kDueGraceS);
}

// ---------------------------------------------------------------------------
// Wake arithmetic
// ---------------------------------------------------------------------------

TEST(AutoModePolicy, WakesBeaconLeadBeforeTheEvent) {
    // The lead exists so the beacon exchange and any pending config land BEFORE
    // the node acts — an edit made an hour ago takes effect on this event.
    EXPECT_EQ(wakeAtEpoch(kNoon, kNoon + 600, /*lead=*/30, /*checkin=*/0),
              kNoon + 570);
}

TEST(AutoModePolicy, AnImminentEventNapsRatherThanSleepingPastIt) {
    // Event closer than the lead: clamp to now+1 so the node naps and wakes to
    // execute. Without this the wake would be computed in the past.
    EXPECT_EQ(wakeAtEpoch(kNoon, kNoon + 10, /*lead=*/30, /*checkin=*/0),
              kNoon + 1);
    EXPECT_EQ(sleepSeconds(kNoon, kNoon + 10, 30, 0), 1u);
}

TEST(AutoModePolicy, TheCheckinIntervalIsACeilingNotAnAlternative) {
    // It bounds how long a hub-side change can sit unseen by a sleeping node,
    // so it must win whenever it is sooner than the event.
    EXPECT_EQ(sleepSeconds(kNoon, kNoon + 40000, /*lead=*/30, /*checkin=*/21600),
              21600u);
    // ...and lose when the event is sooner.
    EXPECT_EQ(sleepSeconds(kNoon, kNoon + 600, /*lead=*/30, /*checkin=*/21600),
              570u);
}

TEST(AutoModePolicy, NoEventAndNoCheckinMeansStayAwake) {
    // Nothing to sleep towards. Sleeping anyway is how a node disappears.
    EXPECT_EQ(sleepSeconds(kNoon, /*next=*/0, 30, /*checkin=*/0), 0u);
}

TEST(AutoModePolicy, NoEventStillHonoursTheCheckin) {
    EXPECT_EQ(sleepSeconds(kNoon, /*next=*/0, 30, /*checkin=*/600), 600u);
}

// ---------------------------------------------------------------------------
// Quiet window
// ---------------------------------------------------------------------------

TEST(AutoModePolicy, QuietWindowComesFromTheConfiguredValue) {
    // post_event_window was configured, CRC'd, pushed, persisted and echoed
    // back — while nothing read it and a hard-coded constant did its job.
    EXPECT_EQ(quietWindowMs(45), 45000u);
}

TEST(AutoModePolicy, QuietWindowIsFlooredBelowTheHubsInterFrameGaps) {
    // The window also holds the node awake THROUGH the handshake. Shorter than
    // the hub's gaps (login ~4 s, push ~2 s later, retransmits 5 s apart) and
    // the node sleeps mid-conversation — F8, reintroduced from YAML.
    EXPECT_EQ(quietWindowMs(2), kQuietWindowMinMs);
    EXPECT_EQ(quietWindowMs(0), kQuietWindowMinMs);
}

// ---------------------------------------------------------------------------
// Double-execution guard
// ---------------------------------------------------------------------------

TEST(AutoModePolicy, AnEntryDoesNotRunTwice) {
    // runDueScheduleEntry() is called at boot AND when the hub goes quiet, so
    // without this an entry replays on every wake for the whole window.
    EXPECT_TRUE(alreadyExecuted(/*due=*/kNoon, /*lastExec=*/kNoon));
    EXPECT_TRUE(alreadyExecuted(kNoon - 60, kNoon));
}

TEST(AutoModePolicy, ALaterEntryStillRuns) {
    EXPECT_FALSE(alreadyExecuted(kNoon + 60, kNoon));
}

TEST(AutoModePolicy, NothingDueIsNotTreatedAsAlreadyExecuted) {
    // due == 0 means "no entry found", which must not be confused with "already
    // done" — the two lead to opposite logging and would mask a real miss.
    EXPECT_FALSE(alreadyExecuted(0, kNoon));
}
