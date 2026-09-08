// ClassAWindows — Mode C receive windows off the node's own transmission.
//
// See docs/test-plan.md section 6.1.

#include <gtest/gtest.h>

#include "ClassAWindows.h"
#include "TimedGrid.h"

using namespace classa;

TEST(ClassAWindows, OffsetsAreFromT0UplinkNotTxDone) {
    // The difference is n_sym(len)*T_sym: 18.4 ms at 25 B, 92.2 ms at 152 B.
    // Anchoring on t_txdone directly would make short frames work and long
    // frames fail — a payload-dependent failure that reads as interference.
    const int64_t t_txdone = 10000000;

    EXPECT_EQ(t0UplinkUs(t_txdone, 25),  t_txdone - 18432);
    EXPECT_EQ(t0UplinkUs(t_txdone, 152), t_txdone - 92160);

    const int64_t d = t0UplinkUs(t_txdone, 25) - t0UplinkUs(t_txdone, 152);
    EXPECT_EQ(d, 73728);
    EXPECT_GT(d, (int64_t) kWindowUs)
        << "the payload-length error alone exceeds a whole window";
}

TEST(ClassAWindows, WindowPositions) {
    const int64_t t0 = 0;
    EXPECT_EQ(rx1OpenUs(t0),  1000000 - 17216);
    EXPECT_EQ(rx1CloseUs(t0), 1000000 - 17216 + 29440);
    EXPECT_EQ(rx2OpenUs(t0),  2000000 - 17216);
    EXPECT_EQ(rx2CloseUs(t0), 2000000 - 17216 + 29440);
}

TEST(ClassAWindows, WindowsDoNotOverlap) {
    EXPECT_LT(rx1CloseUs(0), rx2OpenUs(0));
}

TEST(ClassAWindows, NoClockAgreementRequired) {
    // Inject an arbitrary node-vs-hub clock error. The windows are unchanged,
    // because they are referenced to a transmission the node timed itself.
    // This is the whole reason Class A works for a node that just booted.
    for (int64_t skew : {-5000000LL, 0LL, 5000000LL}) {
        const int64_t t_txdone = 10000000 + skew;
        const int64_t t0 = t0UplinkUs(t_txdone, 60);
        EXPECT_EQ(rx1OpenUs(t0) - t0, 1000000 - 17216);
        EXPECT_EQ(rx2OpenUs(t0) - t0, 2000000 - 17216);
    }
}

// ---------------------------------------------------------------------------
// The wake state machine
// ---------------------------------------------------------------------------

TEST(ClassAWindows, Rx1ThenRx2ThenSleep) {
    WakeState s;
    EXPECT_EQ(nextAction(s), WakeAction::OpenRx1);
    s.rx1_done = true;
    EXPECT_EQ(nextAction(s), WakeAction::OpenRx2);
    s.rx2_done = true;
    EXPECT_EQ(nextAction(s), WakeAction::Sleep);
}

TEST(ClassAWindows, Rx2OnlyIfRx1Empty) {
    WakeState s;
    s.rx1_done = true;
    s.rx1_had_data = true;
    EXPECT_EQ(nextAction(s), WakeAction::Sleep);
}

TEST(ClassAWindows, SleepOkClosesBothWindows) {
    WakeState s;
    s.sleep_ok = true;
    EXPECT_EQ(nextAction(s), WakeAction::Sleep);
}

TEST(ClassAWindows, SleepOkDefaultsToKeepWaiting) {
    // proto3 zero must mean today's behaviour, or a hub that has not been
    // upgraded silently puts every node to sleep early.
    EXPECT_FALSE(WakeState{}.sleep_ok);
    EXPECT_EQ(nextAction(WakeState{}), WakeAction::OpenRx1);
}

// ---------------------------------------------------------------------------
// The negative test that keeps C2 honest
// ---------------------------------------------------------------------------

TEST(ClassAWindows, TodaysHubRepliesMissBothWindows) {
    // An earlier draft of the design claimed D1 ~ 1 s and D2 ~ 2 s "fit the
    // existing behaviour". They do not: the hub replies at +750 ms as a
    // 17-copy burst, so copies land at 750 + 88i ms and both windows fall
    // between copies. This test is what makes that class of claim checkable.
    const int64_t t0 = 0;
    for (uint8_t i = 0; i < kBurstCopies; ++i) {
        EXPECT_FALSE(copyLandsIn(rx1OpenUs(t0), rx1CloseUs(t0), i))
            << "copy " << (int) i << " at " << todaysHubCopyUs(i) << " us";
        EXPECT_FALSE(copyLandsIn(rx2OpenUs(t0), rx2CloseUs(t0), i))
            << "copy " << (int) i << " at " << todaysHubCopyUs(i) << " us";
    }

    // The nearest copies either side, quoted in the design document.
    EXPECT_EQ(todaysHubCopyUs(2), 926000);
    EXPECT_EQ(todaysHubCopyUs(3), 1014000);
    EXPECT_EQ(todaysHubCopyUs(14), 1982000);
    EXPECT_EQ(todaysHubCopyUs(15), 2070000);
}

TEST(ClassAWindows, EvenAFortunateAlignmentIsOnlyAThirdOfTheTime) {
    // A window of 29.44 ms against a copy every 88 ms: at best 33.5 %. A burst
    // cannot serve Class A no matter how the offsets are retuned.
    const double best = (double) kWindowUs / (double) kBurstCopyStrideUs;
    EXPECT_NEAR(best, 0.3345, 1e-4);
    EXPECT_LT(best, 0.5);
}

TEST(ClassAWindows, SingleCopyAtTheRightOffsetWouldWork) {
    // The fix is a single copy placed at D1, which lands mid-window.
    const int64_t t0 = 0;
    const int64_t single = (int64_t) kRx1DelayUs;
    EXPECT_GE(single, rx1OpenUs(t0));
    EXPECT_LT(single, rx1CloseUs(t0));
}

// ---------------------------------------------------------------------------
// C2 — the sequencer, as the node actually steps through it
// ---------------------------------------------------------------------------
//
// The window arithmetic above is pure. What was missing until C2 was anything
// that CALLED it: the node had no source for its own TxDone (the synchronous
// path polled at 20 ms against 29.44 ms windows), so the origin the whole mode
// hangs off did not exist. These check the state machine that now uses it.

TEST(ClassAWindows, TheSequenceIsRx1ThenRx2ThenSleepAndDoesNotRestart) {
    WakeState s;
    EXPECT_EQ(nextAction(s), WakeAction::OpenRx1);

    s.rx1_done = true; s.rx1_had_data = false;
    EXPECT_EQ(nextAction(s), WakeAction::OpenRx2);

    s.rx2_done = true;
    EXPECT_EQ(nextAction(s), WakeAction::Sleep);
    // And it stays there. A sequencer that fell back to OpenRx1 would keep a
    // node awake indefinitely on a link where nothing ever answers.
    EXPECT_EQ(nextAction(s), WakeAction::Sleep);
}

TEST(ClassAWindows, DataInRx1SkipsRx2) {
    WakeState s;
    s.rx1_done = true; s.rx1_had_data = true;
    EXPECT_EQ(nextAction(s), WakeAction::Sleep)
        << "the reply arrived; a second window is 29 ms of receive for nothing";
}

TEST(ClassAWindows, TheTwoWindowsCannotOverlap) {
    // If they could, a frame late for RX1 would be counted against RX2 and the
    // node would sleep believing it had listened twice.
    const int64_t t0 = 1'000'000;
    EXPECT_LT(rx1CloseUs(t0), rx2OpenUs(t0));
    EXPECT_EQ(rx2OpenUs(t0) - rx1OpenUs(t0),
              (int64_t) kRx2DelayUs - (int64_t) kRx1DelayUs);
}

TEST(ClassAWindows, TheOriginIsT0NotTxDone) {
    // Using t_txdone directly is wrong by n_sym(len)*T_sym, which across this
    // fleet's frame sizes ranges from 39 to 92 ms — three times the window
    // width at the top end. This is the mistake the header exists to prevent.
    const int64_t t_txdone = 5'000'000;
    const int64_t t0_short = t0UplinkUs(t_txdone, 25);
    const int64_t t0_long  = t0UplinkUs(t_txdone, 152);

    EXPECT_LT(t0_long, t0_short) << "a longer frame started earlier";
    const int64_t spread = t0_short - t0_long;
    EXPECT_GT(spread, (int64_t) kWindowUs)
        << "the payload-length term alone exceeds a whole window, so ignoring "
           "it would miss the reply outright";
}

TEST(ClassAWindows, TheHubsStampUncertaintyEatsAThirdOfTheGuard) {
    // The hub aims its reply at T0_uplink + kRx1DelayUs, where T0_uplink is
    // Bx's receive stamp. On the poll path that is good to about +/-5 ms, and
    // the guard band either side of the mark is 14.08 ms — so the hub's own
    // uncertainty consumes **36 %** of the budget before clock drift, the
    // node's arm residual or the hub's fire jitter have spent anything.
    //
    // It fits. It fits by 2.8x, not by the order of magnitude the guard was
    // sized for, and that is the honest reading: C2 works on the poll stamp and
    // wants DIO0 wired on the hub. Asserted rather than described so the margin
    // cannot quietly shrink.
    constexpr int64_t kHubStampUncertaintyUs = 5000;   // Bx, nominal 10 ms poll
    EXPECT_LT(kHubStampUncertaintyUs, (int64_t) timedgrid::kGuardUs);

    const int64_t pct = kHubStampUncertaintyUs * 100 / (int64_t) timedgrid::kGuardUs;
    EXPECT_GE(pct, 30) << "if this drops, the stamp got better — re-derive";
    EXPECT_LE(pct, 40) << "and if it rises, C2 is running out of guard";

    // What is left for everything else.
    EXPECT_GT((int64_t) timedgrid::kGuardUs - kHubStampUncertaintyUs, 9000)
        << "drift, arm residual and fire jitter share what remains";
}
