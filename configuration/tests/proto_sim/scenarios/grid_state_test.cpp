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
