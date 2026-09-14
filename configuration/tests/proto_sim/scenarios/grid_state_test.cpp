// GridState — the node's copy of the grid.
//
// The property worth testing hardest is that NO CLOCK CROSSES THE LINK. The
// hub's esp_timer and the node's are unrelated, so the grid is transferred as a
// POSITION and the node solves for its own anchor.
//
// See docs/implementation-plan.md 4.2 and 4.6.

#include <gtest/gtest.h>

#include "GridState.h"

using namespace gridstate;

namespace {
Params good(uint32_t slot = 3) {
    Params p;
    p.slot_index = slot;
    p.beacon_every_rounds = 233;
    p.beacon_slot = timedgrid::kSlotCount - 1;
    return p;
}
}  // namespace

// ---------------------------------------------------------------------------
// The anchor is solved, never transferred
// ---------------------------------------------------------------------------

TEST(GridState, AnchorIsSolvedFromTheFramesDeclaredPosition) {
    const Params p = good(3);
    // The hub says "this frame's T0 is round 7, slot 3". The node measured that
    // T0 at 12 345 678 on ITS OWN clock.
    const int64_t t0_measured = 12345678;
    const int64_t anchor = solveAnchorUs(t0_measured, 7, 3, p);

    EXPECT_EQ(anchor, t0_measured
                        - 7 * (int64_t) timedgrid::kRoundUs
                        - 3 * (int64_t) timedgrid::kSlotPitchUs);

    // Round-trip: the grid built from that anchor puts round 7 back exactly
    // where the frame was heard.
    State st;
    st.active = true; st.params = p; st.anchor_us = anchor;
    EXPECT_EQ(t0ForRound(st, 7), t0_measured);
}

TEST(GridState, TheNodesClockOffsetIsIrrelevant) {
    // Two nodes with wildly different clock origins, hearing the SAME frame,
    // each end up with a grid that puts that frame at the same round and slot.
    const Params p = good(5);
    for (int64_t origin : {0LL, 1000000000LL, -500000000LL}) {
        const int64_t t0 = origin + 4242;
        State st;
        st.active = true; st.params = p;
        st.anchor_us = solveAnchorUs(t0, 11, 5, p);
        EXPECT_EQ(t0ForRound(st, 11), t0) << "origin " << origin;
    }
}

TEST(GridState, SlotSpacingSurvivesTheSolve) {
    const Params p = good(0);
    State st;
    st.active = true; st.params = p;
    st.anchor_us = solveAnchorUs(1000000, 0, 0, p);
    for (uint32_t r = 0; r < 4; ++r)
        EXPECT_EQ(t0ForRound(st, r + 1) - t0ForRound(st, r),
                  (int64_t) timedgrid::kRoundUs);
}

// ---------------------------------------------------------------------------
// Agreement — refuse rather than half-adopt
// ---------------------------------------------------------------------------

TEST(GridState, AGridThatAgreesIsAccepted) {
    EXPECT_EQ(validate(good(3), /*tx_slot=*/3), Refusal::None);
}

TEST(GridState, DisagreementAboutGeometryIsRefused) {
    // A hub and node compiled against different TimedGrid.h constants would
    // each believe in a different pitch and quietly miss every window — a
    // failure that presents as a dead radio. Refusing is far better.
    Params p = good();
    p.slot_count = 16;
    EXPECT_EQ(validate(p, 3), Refusal::SlotCountMismatch);

    p = good(); p.round_us = 1000000;
    EXPECT_EQ(validate(p, 3), Refusal::RoundMismatch);

    p = good(); p.pitch_us = 31250;
    EXPECT_EQ(validate(p, 3), Refusal::PitchMismatch);
}

TEST(GridState, OutOfRangeSlotsAreRefused) {
    Params p = good();
    p.slot_index = timedgrid::kSlotCount;
    EXPECT_EQ(validate(p, 3), Refusal::SlotOutOfRange);

    EXPECT_EQ(validate(good(3), /*tx_slot=*/timedgrid::kSlotCount),
              Refusal::TxSlotOutOfRange);

    p = good(); p.beacon_slot = timedgrid::kSlotCount + 1;
    EXPECT_EQ(validate(p, 3), Refusal::BeaconSlotOutOfRange);
}

TEST(GridState, ABeaconSlotIsOnlyCheckedWhenBeaconsAreOn) {
    Params p = good();
    p.beacon_every_rounds = 0;
    p.beacon_slot = 9999;          // meaningless, but harmless with no beacon
    EXPECT_EQ(validate(p, 3), Refusal::None);
}

// ---------------------------------------------------------------------------
// Next-mark arithmetic
// ---------------------------------------------------------------------------

TEST(GridState, NextT0IsNeverInThePast) {
    State st;
    st.active = true; st.params = good(9);
    st.anchor_us = 0;
    for (int64_t now = 0; now < 3LL * timedgrid::kRoundUs; now += 9999) {
        const int64_t t0 = nextT0Us(st, now);
        EXPECT_GE(t0, now) << "now " << now;
        EXPECT_LE(t0 - now, (int64_t) timedgrid::kRoundUs) << "now " << now;
    }
}

TEST(GridState, BeforeTheFirstT0DoesNotRoundBackwards) {
    // Same truncation trap as the hub's nextT0ForSlotUs: slot 31's first T0 is
    // 1.45 s after the anchor, so any `now` in that window exercises it.
    State st;
    st.active = true; st.params = good(timedgrid::kSlotCount - 1);
    st.anchor_us = 0;
    const int64_t first = t0ForRound(st, 0);
    ASSERT_GT(first, 0);
    for (int64_t now = 0; now < first; now += 10000)
        EXPECT_EQ(nextT0Us(st, now), first) << "now " << now;
}

TEST(GridState, WithoutAGridTheAnswerIsNow) {
    State st;
    EXPECT_FALSE(st.active);
    EXPECT_EQ(nextT0Us(st, 777777), 777777);
}

TEST(GridState, BeaconRoundsIncludeRoundZero) {
    // A node that has just adopted a grid must not wait a full interval for its
    // first beacon window.
    State st;
    st.active = true; st.params = good();
    EXPECT_TRUE(isBeaconRound(st, 0));
    EXPECT_TRUE(isBeaconRound(st, 233));
    EXPECT_FALSE(isBeaconRound(st, 1));

    st.params.beacon_every_rounds = 0;
    EXPECT_FALSE(isBeaconRound(st, 0)) << "beacons off means no beacon rounds";
}

TEST(GridState, ClearReturnsToModeA) {
    State st;
    st.active = true; st.params = good(); st.anchor_us = 12345;
    st.clear();
    EXPECT_FALSE(st.active);
    EXPECT_EQ(st.anchor_us, 0);
}

// ---------------------------------------------------------------------------
// HW-2 — the sweep offset is bench-only.
// ---------------------------------------------------------------------------

TEST(GridState, ASweepOffsetIsRefusedOffTheBench) {
    // It deliberately breaks reception. A field node must keep the correct arm
    // lead whatever the hub asks for.
    Params p = good();
    p.arm_offset_us = 5000;
    EXPECT_EQ(validate(p, 3, /*is_bench_node=*/false), Refusal::SweepOffsetNotAllowed);
    EXPECT_EQ(validate(p, 3, /*is_bench_node=*/true), Refusal::None);
}

TEST(GridState, TheSweepRefusalIsCheckedBeforeGeometry) {
    // Refusing a degrading offset must not depend on the rest of the grid
    // being agreeable, or a malformed frame could smuggle one past.
    Params p = good();
    p.arm_offset_us = 5000;
    p.pitch_us = 31250;                    // also wrong
    EXPECT_EQ(validate(p, 3, false), Refusal::SweepOffsetNotAllowed);
}

TEST(GridState, AZeroOffsetIsAlwaysFine) {
    EXPECT_EQ(validate(good(), 3, /*is_bench_node=*/false), Refusal::None);
}

TEST(GridState, TheOffsetShiftsTheArmInstantAndNothingElse) {
    State st;
    st.active = true; st.params = good(4);
    st.anchor_us = 0;
    const int64_t t0 = t0ForRound(st, 3);

    EXPECT_EQ(armInstantUs(st, t0), t0 - (int64_t) timedgrid::kArmLeadUs);

    st.params.arm_offset_us = -2500;
    EXPECT_EQ(armInstantUs(st, t0), t0 - (int64_t) timedgrid::kArmLeadUs - 2500);
    EXPECT_EQ(t0ForRound(st, 3), t0) << "the grid itself must not move";

    st.params.arm_offset_us = 7000;
    EXPECT_EQ(armInstantUs(st, t0), t0 - (int64_t) timedgrid::kArmLeadUs + 7000);
}

// ---------------------------------------------------------------------------
// B3 — the delay a one-shot timer is armed with
// ---------------------------------------------------------------------------

TEST(GridState, ArmDelayLandsTheRadioOnTheArmInstantMinusTheCallersLead) {
    State st;
    st.active = true; st.params = good(5);
    st.anchor_us = 0;

    const int64_t lead = 1000;
    const int64_t now  = 10'000;                       // well before slot 5's T0
    const int64_t d    = armDelayUs(st, now, lead);

    EXPECT_EQ(now + d + lead, armInstantUs(st, nextT0Us(st, now)))
        << "the timer fires exactly `lead` before the radio must be listening";
}

TEST(GridState, ArmDelayIsNeverZeroOrNegative) {
    // esp_timer_start_once rejects some of those, and a caller that skipped the
    // mark instead would guarantee a miss where arming late might still catch
    // the frame — lateness inside the guard band G is survivable, silence is
    // not.
    State st;
    st.active = true; st.params = good(0);
    st.anchor_us = 0;

    for (int64_t now = -50'000; now < 3LL * timedgrid::kRoundUs; now += 1013)
        EXPECT_GE(armDelayUs(st, now, /*lead=*/1000), 1) << "now " << now;

    // Specifically: an instant already past its own arm point.
    const int64_t t0   = nextT0Us(st, 0);
    const int64_t late = armInstantUs(st, t0) + 500;   // 0.5 ms after arming was due
    EXPECT_EQ(armDelayUs(st, late, /*lead=*/1000), 1);
}

TEST(GridState, ALargerLeadArmsEarlierNotLater) {
    State st;
    st.active = true; st.params = good(7);
    st.anchor_us = 0;
    const int64_t now = 1000;

    const int64_t small = armDelayUs(st, now, 500);
    const int64_t large = armDelayUs(st, now, 5000);
    EXPECT_LT(large, small);
    EXPECT_EQ(small - large, 4500);
}

TEST(GridState, ArmDelayNeverExceedsARound) {
    // A delay longer than a round would mean a mark was skipped.
    State st;
    st.active = true; st.params = good(11);
    st.anchor_us = 500'000;

    for (int64_t now = 600'000; now < 600'000 + 3LL * timedgrid::kRoundUs; now += 7919)
        EXPECT_LE(armDelayUs(st, now, /*lead=*/1000),
                  (int64_t) timedgrid::kRoundUs) << "now " << now;
}

TEST(GridState, TheSweepOffsetMovesTheArmDelayWithIt) {
    // HW-2 steps arm_offset_us to find the reception edge. If the offset did
    // not reach the timer the sweep would measure nothing at all.
    State a; a.active = true; a.params = good(2); a.anchor_us = 0;
    State b = a; b.params.arm_offset_us = 3000;

    const int64_t now = 5000;
    EXPECT_EQ(armDelayUs(b, now, 1000) - armDelayUs(a, now, 1000), 3000);
}

TEST(GridState, WithoutAGridTheDelayIsImmediateNotAStall) {
    State st;
    EXPECT_FALSE(st.active);
    EXPECT_EQ(armDelayUs(st, 777'777, 1000), 1)
        << "no grid means the caller should not be asking; stalling is worse "
           "than arming early";
}

// ---------------------------------------------------------------------------
// The beacon window (section 4.4)
//
// The 32 private windows sit at 32 different phases, so one broadcast cannot
// reach them all: the beacon has its own slot, on beacon rounds only, and every
// node opens it. Without it a node holds phase for resyncMaxS after each
// addressed frame — at 3.5 commands/day that is 2.9 % of the day on the grid,
// and no battery saving at all.
// ---------------------------------------------------------------------------

namespace {
State gridded(uint32_t slot = 3, int64_t anchor = 1'000'000) {
    State st;
    st.active = true;
    st.params = good(slot);
    st.anchor_us = anchor;
    return st;
}
}  // namespace

TEST(GridState, BeaconMarksSitInTheBeaconSlotNotTheNodes) {
    const State st = gridded(3);
    // Same round, two different phases: the beacon's slot and this node's.
    EXPECT_EQ(beaconT0ForRound(st, 5) - t0ForRound(st, 5),
              (int64_t) (st.params.beacon_slot - st.params.slot_index)
                  * (int64_t) timedgrid::kSlotPitchUs);
}

TEST(GridState, TheNextBeaconIsOnABeaconRound) {
    const State st = gridded(3);
    const int64_t t0 = nextBeaconT0Us(st, st.anchor_us + 1);
    ASSERT_GT(t0, 0);
    // It must be a mark of the beacon slot, and its round a multiple of the
    // cadence — those are two different statements and both have to hold.
    const int64_t rel = t0 - st.anchor_us
                      - (int64_t) st.params.beacon_slot * (int64_t) timedgrid::kSlotPitchUs;
    ASSERT_EQ(rel % (int64_t) timedgrid::kRoundUs, 0);
    const uint32_t round = (uint32_t) (rel / (int64_t) timedgrid::kRoundUs);
    EXPECT_EQ(round % st.params.beacon_every_rounds, 0u);
    EXPECT_TRUE(isBeaconRound(st, round));
}

TEST(GridState, NoCadenceMeansNoBeaconRatherThanInstantZero) {
    State st = gridded(3);
    st.params.beacon_every_rounds = 0;
    EXPECT_EQ(nextBeaconT0Us(st, st.anchor_us + 1), 0)
        << "0 is the signal that there is no beacon — never an instant";
    // And the caller must then behave exactly as it did before beacons existed.
    const NextWindow w = nextWindow(st, st.anchor_us + 1, /*skip_own=*/false);
    EXPECT_EQ(w.kind, WindowKind::Own);
    EXPECT_EQ(w.t0_us, nextT0Us(st, st.anchor_us + 1));
}

TEST(GridState, ASkippingNodeStillOpensTheBeaconWindow) {
    // The one that must not be got backwards. The beacon is what GRANTS and
    // REVOKES the skip permission and what expires it when it goes missing, so
    // a node that skipped it too could never learn it has traffic waiting.
    const State st = gridded(3);
    const int64_t now = st.anchor_us + 1;

    const NextWindow skipped = nextWindow(st, now, /*skip_own=*/true);
    EXPECT_EQ(skipped.kind, WindowKind::Beacon);
    EXPECT_EQ(skipped.t0_us, nextBeaconT0Us(st, now));

    // And it is genuinely further away than the private mark it replaced —
    // otherwise this test would pass on a grid where they coincide.
    const NextWindow listening = nextWindow(st, now, /*skip_own=*/false);
    EXPECT_EQ(listening.kind, WindowKind::Own);
    EXPECT_LT(listening.t0_us, skipped.t0_us);
}

TEST(GridState, TheBeaconWinsWhenItComesFirst) {
    // A listening node opens whichever comes first, so it hears the beacon on
    // beacon rounds without losing its own marks on every other round.
    const State st = gridded(3);
    // Just before a beacon mark: the beacon is next, not this node's slot.
    const int64_t beacon = nextBeaconT0Us(st, st.anchor_us + 1);
    const NextWindow w = nextWindow(st, beacon - 1000, /*skip_own=*/false);
    EXPECT_EQ(w.kind, WindowKind::Beacon);
    EXPECT_EQ(w.t0_us, beacon);
}

TEST(GridState, TheArmDelayReportsWhichWindowItIsFor) {
    // The caller has to know: a beacon window that closes empty is not a missed
    // MARK, and counting it would feed a demotion the node has not earned.
    const State st = gridded(3);
    WindowKind kind = WindowKind::Own;
    const int64_t beacon = nextBeaconT0Us(st, st.anchor_us + 1);

    nextWindowArmDelayUs(st, beacon - 1000, (int64_t) timedgrid::kArmLeadUs, false, kind);
    EXPECT_EQ(kind, WindowKind::Beacon);

    nextWindowArmDelayUs(st, st.anchor_us + 1, (int64_t) timedgrid::kArmLeadUs, false, kind);
    EXPECT_EQ(kind, WindowKind::Own);
}

TEST(GridState, AWindowThatHasOpenedIsNeverArmedAgain) {
    // Measured 2026-09-14 on node 2 (fw 1.0.68): the receive task came round a
    // few ms after opening a window, before its T0, and armed the same mark again
    // at 1 us. It heard none of ~120 marks the hub placed.
    const State st = gridded(3);
    const int64_t lead = 1000;
    const int64_t own  = nextT0Us(st, st.anchor_us + 5 * (int64_t) timedgrid::kRoundUs);
    // Just after the window opened: the arm instant has passed, T0 has not.
    const int64_t now  = armInstantUs(st, own) + 2000;
    ASSERT_LT(now, own);

    WindowKind kind = WindowKind::Own;
    int64_t aimed = 0;
    nextWindowArmDelayUs(st, now, lead, false, kind, /*opened_t0_us=*/0, &aimed);
    ASSERT_EQ(aimed, own) << "precondition: without the record, the open mark is found again";

    const int64_t d = nextWindowArmDelayUs(st, now, lead, false, kind, /*opened_t0_us=*/own, &aimed);
    EXPECT_EQ(aimed, nextT0Us(st, own + 1)) << "the NEXT round's mark";
    EXPECT_EQ(kind, WindowKind::Own);
    EXPECT_EQ(d, armInstantUs(st, aimed) - lead - now) << "and the delay still runs from now";
}

TEST(GridState, AnArmedMarkThatHasNotOpenedIsStillTheOneToArm) {
    // A stale wake brings the task round before the one-shot fires. Nothing has
    // opened, so the record is the PREVIOUS window and the pending mark stands.
    const State st = gridded(3);
    const int64_t own      = nextT0Us(st, st.anchor_us + 5 * (int64_t) timedgrid::kRoundUs);
    const int64_t previous = own - (int64_t) timedgrid::kRoundUs;
    const int64_t now      = armInstantUs(st, own) - 400000;

    WindowKind kind = WindowKind::Own;
    int64_t aimed = 0;
    nextWindowArmDelayUs(st, now, 1000, false, kind, /*opened_t0_us=*/previous, &aimed);
    EXPECT_EQ(aimed, own);
}

TEST(GridState, AReAnchorInsideTheGuardDoesNotBringTheOpenedWindowBack) {
    // A beacon heard in that window re-anchors the grid, moving the predicted T0
    // of the very mark that just opened by up to the guard. Searching from
    // "opened + 1 us" would find it again the moment the shift was positive.
    State st = gridded(3);
    const int64_t own = nextT0Us(st, st.anchor_us + 5 * (int64_t) timedgrid::kRoundUs);
    st.anchor_us += (int64_t) timedgrid::kGuardUs;   // the whole guard, later
    const int64_t shifted = own + (int64_t) timedgrid::kGuardUs;
    const int64_t now = own - 5000;

    WindowKind kind = WindowKind::Own;
    int64_t aimed = 0;
    nextWindowArmDelayUs(st, now, 1000, false, kind, /*opened_t0_us=*/own, &aimed);
    EXPECT_GT(aimed, shifted) << "the re-anchored copy of the opened mark is not a new window";
    EXPECT_EQ(aimed, shifted + (int64_t) timedgrid::kRoundUs);
}

TEST(GridState, RoundNumbersAreRecoverableFromAMark) {
    // The number both ends must agree on. The hub declares the round it
    // transmits in precisely so this inverse works out to the same value there.
    const State st = gridded(3);
    for (uint32_t r : {0u, 1u, 232u, 233u, 1000u})
        EXPECT_EQ(roundForT0(st, t0ForRound(st, r)), r);
}

// ---------------------------------------------------------------------------
// Re-anchoring from a beacon
// ---------------------------------------------------------------------------

TEST(GridState, ABeaconMayCorrectDriftButNotWalkTheAnchor) {
    // A beacon is broadcast, so it cannot be sealed with a per-node session
    // key. The bound is what makes it safe to act on: real drift is orders of
    // magnitude smaller than the guard — one round at +/-20 ppm is 30 us
    // against 14080 — so anything larger is a foreign frame or a node that has
    // already lost the grid, and both are cases for demoting rather than for
    // chasing the anchor.
    EXPECT_TRUE(reanchorIsSane(0, timedgrid::kGuardUs));
    EXPECT_TRUE(reanchorIsSane(30, timedgrid::kGuardUs));
    EXPECT_TRUE(reanchorIsSane(-30, timedgrid::kGuardUs));
    EXPECT_TRUE(reanchorIsSane((int64_t) timedgrid::kGuardUs, timedgrid::kGuardUs));
    EXPECT_TRUE(reanchorIsSane(-(int64_t) timedgrid::kGuardUs, timedgrid::kGuardUs));

    EXPECT_FALSE(reanchorIsSane((int64_t) timedgrid::kGuardUs + 1, timedgrid::kGuardUs));
    EXPECT_FALSE(reanchorIsSane(-(int64_t) timedgrid::kGuardUs - 1, timedgrid::kGuardUs));
    // A whole slot out is the case that matters: adopting it would move this
    // node onto its neighbour's window.
    EXPECT_FALSE(reanchorIsSane((int64_t) timedgrid::kSlotPitchUs, timedgrid::kGuardUs));
}

// ---------------------------------------------------------------------------
// Placing the uplink (section 4.3)
//
// ulOffsetUs was on the wire from the beginning and nothing read it, so the
// hub's in-slot measurement was testing a number the node had never heard of.
// These pin the arithmetic that makes the field mean what it says.
// ---------------------------------------------------------------------------

namespace {
// The lead the node actually uses: CAD + preamble, with the two unmeasured
// terms at zero. Restated here rather than imported so a change to either end
// shows up as a failing test rather than as an assertion that agrees with
// whatever the code does.
constexpr int64_t kLead = (int64_t) loratiming::kPreambleToT0Us
                        + (int64_t) loratiming::kCadUs;
// The bound the node passes: the random backoff this aim replaces, at its
// worst case (LoraInterface::maxUplinkAimWaitUs — 29 ms x 10).
constexpr int64_t kMaxWait = 290000;
State aimable(uint32_t slot = 3, int64_t anchor = 1'000'000) {
    State st = gridded(slot, anchor);
    st.params.ul_offset_us = 60000;   // LORAListener::kUplinkOffsetUs
    return st;
}
}  // namespace

TEST(GridState, AnAimedUplinkLandsOnTheMarkTheHubSubtracts) {
    const State st = aimable(3);
    // The hub finds the mark by subtracting kUplinkOffsetUs from the arriving
    // T0 (noteUplinkPlacement_). The aim has to be the exact inverse or the two
    // ends disagree about what "in slot" means.
    const int64_t now = t0ForRound(st, 0) - 1000;
    const UplinkAim a = aimUplink(st, now, kLead, kMaxWait);
    ASSERT_TRUE(a.aimed);
    EXPECT_EQ(a.t0_us - (int64_t) st.params.ul_offset_us, t0ForRound(st, 0));
    EXPECT_EQ(a.cad_start_us, a.t0_us - kLead);
    EXPECT_GE(a.cad_start_us, now);
}

TEST(GridState, TheAimIsInsideTheHubsInSlotBand) {
    // The whole point: an uplink placed this way passes the hub's own test.
    // The band is the same guard the node's phase tracking uses.
    const State st = aimable(7);
    const UplinkAim a =
        aimUplink(st, t0ForRound(st, 0) - 5000, kLead, kMaxWait);
    ASSERT_TRUE(a.aimed);
    const int64_t err = (a.t0_us - (int64_t) st.params.ul_offset_us) - t0ForRound(st, 0);
    EXPECT_LE(err, (int64_t) timedgrid::kGuardUs);
    EXPECT_GE(err, -(int64_t) timedgrid::kGuardUs);
    EXPECT_EQ(err, 0);
}

TEST(GridState, AMarkAlreadyPastIsNotAimedAt) {
    // The next mark is a whole ROUND away, and holding an ack for 1.5 s to
    // place it trades the thing the user notices for a statistic. Declining is
    // the answer, and the caller then sends the old way.
    const State st = aimable(3);
    const int64_t just_missed = t0ForRound(st, 0) + (int64_t) st.params.ul_offset_us;
    const UplinkAim a =
        aimUplink(st, just_missed, kLead, kMaxWait);
    EXPECT_FALSE(a.aimed);
    EXPECT_EQ(a.cad_start_us, 0);
}

TEST(GridState, AnUplinkNeverWaitsLongerThanTheBackoffItReplaces) {
    // The bound that makes this safe to ship: honouring the offset can only
    // make an uplink EARLIER than the 29-290 ms random backoff, never later.
    const State st = aimable(3);
    for (int64_t off = -2'000'000; off <= 2'000'000; off += 7919) {
        const UplinkAim a =
            aimUplink(st, t0ForRound(st, 0) + off, kLead, kMaxWait);
        if (!a.aimed) continue;
        const int64_t wait = a.cad_start_us - (t0ForRound(st, 0) + off);
        EXPECT_GE(wait, 0);
        EXPECT_LE(wait, (int64_t) kMaxWait);
    }
}

TEST(GridState, NoGridAndNoPublishedOffsetBothDecline) {
    // Two independent refusals, and the second is the one that matters for
    // rollout: a hub that has not published an offset gets exactly the
    // behaviour that shipped before, on every node, with no flag to set.
    State no_grid = aimable(3);
    no_grid.active = false;
    EXPECT_FALSE(aimUplink(no_grid, 0, kLead, kMaxWait).aimed);

    State no_offset = gridded(3);      // ul_offset_us defaults to 0
    ASSERT_EQ(no_offset.params.ul_offset_us, 0u);
    EXPECT_FALSE(aimUplink(no_offset, 0, kLead, kMaxWait).aimed);
}

TEST(GridState, EveryAimedMarkIsOneOfThisNodesOwn) {
    // Not the beacon slot's, and not a neighbour's. An aim that drifted onto
    // another slot would be a systematic collision — 32 nodes transmitting into
    // one window — which is worse than the unslotted uplink it replaces.
    const State st = aimable(11);
    for (int64_t off = -3'000'000; off <= 3'000'000; off += 4001) {
        const UplinkAim a =
            aimUplink(st, t0ForRound(st, 0) + off, kLead, kMaxWait);
        if (!a.aimed) continue;
        const int64_t mark = a.t0_us - (int64_t) st.params.ul_offset_us;
        EXPECT_EQ((mark - st.anchor_us) % (int64_t) st.params.round_us,
                  (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us);
    }
}

TEST(GridState, TheAckCaseIsTheOneThatHasToWork) {
    // The load-bearing case, and the reason the bound is the backoff rather
    // than something tidier. The hub transmits AT this node's mark; a 60 B
    // downlink puts RxDone at mark + 38 912 us, and the node asks a moment
    // later. The published 60 ms offset is then ~17 ms away — comfortably
    // inside the bound, so an ack is placed without ever paying for the wait.
    const State st = aimable(3);
    const int64_t mark    = t0ForRound(st, 0);
    const int64_t rx_done = mark + (int64_t) loratiming::t0ToRxDoneUs(60);
    ASSERT_EQ(rx_done - mark, 38912);

    const UplinkAim a = aimUplink(st, rx_done, kLead, kMaxWait);
    ASSERT_TRUE(a.aimed);
    EXPECT_EQ(a.t0_us - (int64_t) st.params.ul_offset_us, mark);   // THIS round
    EXPECT_LT(a.cad_start_us - rx_done, 20000);
}

TEST(GridState, TurnaroundLongerThanTheOffsetMissesEveryMark) {
    // HW-7's number showing itself. If the node cannot build and hand over a
    // reply within ulOffsetUs of its mark, the instant is always in the past
    // and the next one is a round away — so the aim declines every time, and
    // the counter the dispatcher keeps is what says so. Silence here would look
    // exactly like the offset working.
    const State st = aimable(3);
    const int64_t mark = t0ForRound(st, 0);
    for (int64_t turnaround : {70'000, 100'000, 500'000})
        EXPECT_FALSE(aimUplink(st, mark + turnaround, kLead, kMaxWait).aimed);
}

// ---------------------------------------------------------------------------
// MAC-0 clock discipline: the rate term
//
// MEASURED 2026-09-13: node 2 counts +60 ppm against the hub under the production
// power profile (auto light sleep), +9 with sleep disabled. A grid that steps in
// whole hub-clock rounds leaves the +/-14 080 us guard in about 235 s at +60 ppm,
// inside one beacon interval. rate_ppb stretches every hub-clock span onto the
// node clock; these tests pin that it does so exactly, and that at rate 0 nothing
// changes at all.
// ---------------------------------------------------------------------------

namespace {
State withRate(int32_t rate_ppb, uint32_t slot = 3, int64_t anchor = 1'000'000) {
    State st = aimable(slot, anchor);
    st.rate_ppb = rate_ppb;
    return st;
}
}  // namespace

TEST(GridStateRate, ZeroRateIsBitIdenticalToTheFixedRoundGrid) {
    // The whole change must be invisible until something sets a rate: every
    // test above and every node in the field runs at rate 0 today.
    const State st = withRate(0);
    const int64_t R = st.params.round_us, P = st.params.pitch_us;
    const int64_t base = st.anchor_us + (int64_t) st.params.slot_index * P;
    for (uint32_t r : {0u, 1u, 2u, 233u, 600u, 57600u})
        EXPECT_EQ(t0ForRound(st, r), base + (int64_t) r * R) << "round " << r;
    for (int64_t now : {base - 5, base, base + 1, base + R - 1, base + R, base + 7 * R + 3})
    {
        const int64_t d = now - base;
        const int64_t expect = (d <= 0) ? base : base + ((d + R - 1) / R) * R;
        EXPECT_EQ(nextT0Us(st, now), expect) << "now " << now;
    }
    for (int64_t t0 : {base, base + 1, base + R - 1, base + 5 * R})
        EXPECT_EQ(roundForT0(st, t0), (uint32_t) ((t0 - base) / R)) << "t0 " << t0;
}

TEST(GridStateRate, RoundAndT0StayExactInversesWithARate) {
    for (int32_t rate : {60000, -60000, 200000, -200000})
    {
        const State st = withRate(rate);
        for (uint32_t r : {1u, 2u, 599u, 600u, 57600u})
        {
            const int64_t t0 = t0ForRound(st, r);
            EXPECT_EQ(roundForT0(st, t0), r)       << "rate " << rate << " round " << r;
            EXPECT_EQ(roundForT0(st, t0 - 1), r - 1) << "rate " << rate << " just before round " << r;
        }
    }
}

TEST(GridStateRate, SixtyPpmMovesRound600ByExactly54Ms) {
    // 600 rounds x 1.5 s = 900 s of hub time; 60 ppm of that is 54 000 us. This is
    // the drift the bench measured over a 900 s run with no correction.
    const int64_t nominal   = t0ForRound(withRate(0), 600);
    const int64_t corrected = t0ForRound(withRate(60000), 600);
    const int64_t span = 600LL * (int64_t) withRate(0).params.round_us
                       + (int64_t) withRate(0).params.slot_index * (int64_t) withRate(0).params.pitch_us;
    EXPECT_EQ(corrected - nominal, (span * 60000) / 1000000000LL);
    EXPECT_NEAR((double) (corrected - nominal), 54000.0, 20.0)
        << "positive rate: the node counts more microseconds, so its marks come later";
}

TEST(GridStateRate, TheNextMarkIsNeverInThePastAndNeverSkipsOne) {
    // Probed AT each mark and one microsecond either side of it, because a search
    // seeded from a first-order inverse is exactly the kind of code that is right
    // everywhere except at the boundary.
    for (int32_t rate : {60000, -60000, 200000, -200000})
    {
        const State st = withRate(rate);
        for (uint32_t r : {1u, 2u, 3u, 600u, 601u, 57600u})
        {
            const int64_t mark = t0ForRound(st, r);
            for (int64_t now : {mark - 1, mark, mark + 1})
            {
                const int64_t next = nextT0Us(st, now);
                EXPECT_GE(next, now) << "rate " << rate << " round " << r;
                const uint32_t nr = roundForT0(st, next);
                EXPECT_EQ(t0ForRound(st, nr), next) << "the answer must be a mark";
                if (nr > 0)
                    EXPECT_LT(t0ForRound(st, nr - 1), now) << "and the FIRST mark at or after now";
            }
        }
    }
}

TEST(GridStateRate, TheNextBeaconIsNeverInThePastAndNeverSkipsOne) {
    for (int32_t rate : {60000, -60000})
    {
        const State st = withRate(rate);
        const uint32_t every = st.params.beacon_every_rounds;
        ASSERT_GT(every, 0u);
        for (uint32_t k : {1u, 2u, 40u})
        {
            const int64_t mark = beaconT0ForRound(st, k * every);
            for (int64_t now : {mark - 1, mark, mark + 1})
            {
                const int64_t next = nextBeaconT0Us(st, now);
                EXPECT_GE(next, now) << "rate " << rate << " beacon " << k;
                EXPECT_TRUE(next == mark || next == beaconT0ForRound(st, (k + 1) * every))
                    << "the answer must be this beacon or the next one, not a later one";
            }
        }
    }
}

TEST(GridStateRate, AnAimedUplinkLandsOnAStretchedMark) {
    const State st = withRate(60000);
    const int64_t R = st.params.round_us;
    const int64_t offset = (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us
                         + (int64_t) st.params.ul_offset_us;
    // Deep into the run, where a fixed-round aim would be ~54 ms off.
    const int64_t now = t0ForRound(st, 600) + 5000;
    const UplinkAim a = aimUplink(st, now, kLead, kMaxWait);
    ASSERT_TRUE(a.aimed);
    EXPECT_GE(a.cad_start_us, now) << "never aimed into the past";
    bool on_mark = false;
    for (int64_t n = 595; n <= 605; ++n)
        if (a.t0_us == st.anchor_us + stretchUs(st, n * R + offset)) on_mark = true;
    EXPECT_TRUE(on_mark) << "the aim must be an uplink mark on the node clock";
}

TEST(GridStateRate, TheRateAwareAnchorSolveRoundTrips) {
    for (int32_t rate : {0, 60000, -60000})
    {
        State st = withRate(rate, /*slot=*/5, /*anchor=*/7'777'777);
        const uint32_t tx_round = 432;
        const int64_t t0 = t0ForRound(st, tx_round);
        EXPECT_EQ(solveAnchorUs(t0, tx_round, st.params.slot_index, st.params, rate), st.anchor_us)
            << "rate " << rate << ": the anchor recovered from a frame must be the one that predicted it";
    }
}

TEST(GridStateRate, TheLearningHelpersHoldTheirBoundaries) {
    const uint32_t pitch = timedgrid::kSlotPitchUs;   // 46 875 us
    EXPECT_TRUE(beaconErrLearnable(23437, pitch));
    EXPECT_TRUE(beaconErrLearnable(-23437, pitch));
    EXPECT_FALSE(beaconErrLearnable(23438, pitch));
    EXPECT_FALSE(beaconErrLearnable(-23438, pitch));

    EXPECT_EQ(residualRatePpb(5000, 59LL * 1000000), 0)
        << "under a minute, drift cannot be told from timestamp jitter";
    EXPECT_EQ(residualRatePpb(20970, 349500000), 60000)
        << "20 970 us over one 5.8 min beacon interval is +60 ppm";
    EXPECT_EQ(residualRatePpb(-20970, 349500000), -60000);

    EXPECT_EQ(clampRatePpb(300000), 200000);
    EXPECT_EQ(clampRatePpb(-300000), -200000);
    EXPECT_EQ(clampRatePpb(60000), 60000);
}
