// TimedGrid — the Mode B slot geometry.
//
// Two kinds of assertion live here. The first pins arithmetic that is easy to
// get right and catastrophic to get wrong (the pitch, the guard, the anchor).
// The second is written as a FUNCTION of the node turnaround, which has never
// been measured — so when HW-7 returns a number, one constant changes and these
// tests say whether the geometry survived it.
//
// See docs/test-plan.md section 5.2.

#include <gtest/gtest.h>

#include "TimedGrid.h"

using namespace timedgrid;

namespace {
// The turnaround the design assumes. It budgets ZERO for DRAIN — a per-byte
// FIFO read, protobuf unpack, AES-GCM decrypt and three queue hops on a CPU
// scaling down to 40 MHz — so it is optimistic, and named as an assumption.
constexpr uint32_t kAssumedTurnaroundUs = 20000;
}

// ---------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------

TEST(TimedGrid, SlotPitchIsExact) {
    EXPECT_EQ(kSlotPitchUs, 46875u);
    EXPECT_EQ(kRoundUs % kSlotCount, 0u);   // no remainder to accumulate
    EXPECT_EQ(kSlotPitchUs * kSlotCount, kRoundUs);
}

TEST(TimedGrid, WindowSpan) {
    EXPECT_EQ(kWindowUs, 29440u);
    EXPECT_EQ(kArmLeadUs, 17216u);

    const int64_t t0 = 1'000'000;
    EXPECT_EQ(windowOpenUs(t0),  t0 - 17216);
    EXPECT_EQ(windowCloseUs(t0), t0 + 12224);   // 29440 - 17216
}

TEST(TimedGrid, AdjacentWindowsAreClear) {
    for (uint32_t k = 0; k + 1 < kSlotCount; ++k) {
        const int64_t close_k   = windowCloseUs(t0ForSlot(0, 0, k));
        const int64_t open_next = windowOpenUs(t0ForSlot(0, 0, k + 1));
        EXPECT_GT(open_next, close_k) << "slots " << k << "/" << k + 1;
        EXPECT_EQ(open_next - close_k, (int64_t) kInterWindowGapUs);
    }
    EXPECT_EQ(kInterWindowGapUs, 17435u);
}

TEST(TimedGrid, SlotsAreOrderedAndDoNotWrapWithinARound) {
    for (uint32_t k = 0; k + 1 < kSlotCount; ++k)
        EXPECT_LT(t0ForSlot(0, 0, k), t0ForSlot(0, 0, k + 1));
    EXPECT_LT(t0ForSlot(0, 0, kSlotCount - 1), t0ForSlot(0, 1, 0));
}

TEST(TimedGrid, RoundCounterDoesNotOverflowAtThirtyTwoBits) {
    // round * kRoundUs overflows a uint32 after 2863 rounds — 48 minutes — even
    // though a uint32 round counter itself would last 204 years. The widening
    // cast inside t0ForSlot is what this test exists to protect.
    const uint32_t round = 100000;                    // ~41 hours in
    const int64_t expected = (int64_t) round * kRoundUs;
    EXPECT_EQ(t0ForSlot(0, round, 0), expected);
    EXPECT_GT(t0ForSlot(0, round, 0), (int64_t) UINT32_MAX);
}

TEST(TimedGrid, AnchorIsAnOffsetNotAPhase) {
    // Moving the anchor moves every slot by the same amount and changes no
    // relative geometry. This is what "set once, never moved" buys.
    const int64_t a = 987'654'321;
    for (uint32_t k = 0; k < kSlotCount; ++k)
        EXPECT_EQ(t0ForSlot(a, 7, k) - t0ForSlot(0, 7, k), a);
}

// ---------------------------------------------------------------------------
// The guard band
// ---------------------------------------------------------------------------

TEST(TimedGrid, GuardFromSymbolTimeout) {
    // G = (N*T_sym - T_detect) / 2 = (29440 - 1280) / 2
    EXPECT_EQ(kGuardUs, 14080u);
}

TEST(TimedGrid, EarlyAndLateAreSymmetric) {
    EXPECT_TRUE(phaseErrorIsCaught(0));
    EXPECT_TRUE(phaseErrorIsCaught((int32_t) kGuardUs));
    EXPECT_TRUE(phaseErrorIsCaught(-(int32_t) kGuardUs));
    EXPECT_FALSE(phaseErrorIsCaught((int32_t) kGuardUs + 1));
    EXPECT_FALSE(phaseErrorIsCaught(-(int32_t) kGuardUs - 1));
}

TEST(TimedGrid, BeaconIntervalCeiling) {
    // The whole beacon cadence is guard / clock error.
    EXPECT_EQ(maxResyncIntervalS(20), 704u);    // unmeasured crystal spec, 11.7 min
    EXPECT_EQ(maxResyncIntervalS(2), 7040u);    // measured to +/-2 ppm, 117 min

    // The design operates at HALF these, for ppm uncertainty and one lost
    // beacon. Assert the relationship rather than the rounded minutes.
    EXPECT_GE(maxResyncIntervalS(20) / 2, 348u);   // 5.8 min
    EXPECT_GE(maxResyncIntervalS(2) / 2, 3480u);   // 58 min
}

TEST(TimedGrid, GuardScalesWithTheWindowNotWithTheSlot) {
    // Widening the window is the documented fallback while sync is stale
    // (symTimeout is a runtime register write). At 200 symbols the guard is
    // 24.96 ms and holds 20 ppm for 1248 s = 20.8 min.
    constexpr uint32_t wide_window = 200 * kSymbolUs;
    constexpr uint32_t wide_guard  = (wide_window - kDetectUs) / 2;
    EXPECT_EQ(wide_window, 51200u);
    EXPECT_EQ(wide_guard, 24960u);
    EXPECT_EQ(wide_guard / 20, 1248u);
}

// ---------------------------------------------------------------------------
// Servable slots — parameterised on the unmeasured turnaround
// ---------------------------------------------------------------------------

TEST(TimedGrid, HubOccupancyMatchesThePlansTable) {
    EXPECT_EQ(hubOccupancyEndUs(25,  kAssumedTurnaroundUs), 60000u);
    EXPECT_EQ(hubOccupancyEndUs(60,  kAssumedTurnaroundUs), 80480u);
    EXPECT_EQ(hubOccupancyEndUs(152, kAssumedTurnaroundUs), 133728u);
}

TEST(TimedGrid, ServableSlotsAtTheAssumedTurnaround) {
    EXPECT_EQ(nextServableSlotDelta(25,  kAssumedTurnaroundUs), 2u);
    EXPECT_EQ(nextServableSlotDelta(60,  kAssumedTurnaroundUs), 3u);
    EXPECT_EQ(nextServableSlotDelta(152, kAssumedTurnaroundUs), 4u);

    EXPECT_EQ(nodesPerRound(25,  kAssumedTurnaroundUs), 16u);
    EXPECT_EQ(nodesPerRound(60,  kAssumedTurnaroundUs), 10u);
    EXPECT_EQ(nodesPerRound(152, kAssumedTurnaroundUs), 8u);
}

TEST(TimedGrid, SensitivityToTheTurnaroundThatWasNeverMeasured) {
    // The headline (k+3, 10 nodes/round) is comfortable. The ack case is not:
    // it breaks at 36.5 ms, and the assumed value is 20 ms with ZERO budgeted
    // for DRAIN. If HW-7 comes back above 36.5 ms, this is the number that
    // moved, and this test is where it shows.
    EXPECT_EQ(maxTurnaroundForDelta(60,  3), 62929u);
    EXPECT_EQ(maxTurnaroundForDelta(152, 4), 56556u);
    EXPECT_EQ(maxTurnaroundForDelta(25,  2), 36534u);

    // Just inside and just outside each edge.
    EXPECT_EQ(nextServableSlotDelta(25, 36534), 2u);
    EXPECT_EQ(nextServableSlotDelta(25, 36535), 3u);
    EXPECT_EQ(nextServableSlotDelta(60, 62929), 3u);
    EXPECT_EQ(nextServableSlotDelta(60, 62930), 4u);
}

TEST(TimedGrid, ServableDeltaIsMonotonicInTurnaround) {
    uint32_t prev = 0;
    for (uint32_t t = 0; t <= 200000; t += 1000) {
        const uint32_t d = nextServableSlotDelta(60, t);
        EXPECT_GE(d, prev) << "turnaround " << t;
        prev = d;
    }
}

// ---------------------------------------------------------------------------
// The beacon
// ---------------------------------------------------------------------------

TEST(TimedGrid, BeaconNeedsOneClearSlot) {
    EXPECT_EQ(beaconClearSlots(), 1u);

    // The failure it prevents, stated as arithmetic: a beacon at slot b's T0
    // ends after slot b+1's window has already opened.
    const int64_t t0_b = t0ForSlot(0, 0, 0);
    const int64_t beacon_end = t0_b + (int64_t) t0ToRxDoneUs(kBeaconPayloadBytes);
    const int64_t next_open  = windowOpenUs(t0ForSlot(0, 0, 1));
    EXPECT_GT(beacon_end, next_open)
        << "a beacon at slot b blinds slot b+1 on every beacon round";
}

// ---------------------------------------------------------------------------
// One window per round, and why it is not a preference
// ---------------------------------------------------------------------------

TEST(TimedGrid, WindowsPerRoundMustBeOneAt32Slots) {
    EXPECT_EQ(kWindowsPerRound, 1u);
    EXPECT_FALSE(secondWindowFitsInGap());
    EXPECT_GT(kWindowUs, kInterWindowGapUs);
}

TEST(TimedGrid, NoOffsetAdmitsADisjointSecondWindow) {
    // Exhaustive over every 1 ms offset in the round: a second window always
    // overlaps SOME node's primary window. Not a probabilistic statement.
    for (uint32_t off = 1000; off < kRoundUs; off += 1000) {
        const int64_t s_open  = (int64_t) off - (int64_t) kArmLeadUs;
        const int64_t s_close = s_open + kWindowUs;

        bool clear = true;
        for (uint32_t k = 0; k < kSlotCount && clear; ++k) {
            // The PRIMARY windows are what tile the round, so they are the
            // ones that repeat: slot k exists in every round, and a second
            // window near a round boundary must be checked against the
            // neighbouring round's slots too.
            for (int r = -1; r <= 1; ++r) {
                const int64_t p_open  = windowOpenUs(t0ForSlot(0, 0, k))
                                      + (int64_t) r * kRoundUs;
                const int64_t p_close = p_open + kWindowUs;
                if (s_open < p_close && p_open < s_close) { clear = false; break; }
            }
        }
        EXPECT_FALSE(clear) << "offset " << off << " us appeared disjoint";
    }
}

TEST(TimedGrid, The750msProposalIsExactlySixteenSlots) {
    // Not merely overlapping: 750000 / 46875 = 16 exactly, so every node's
    // second window lands on node (k+16)'s primary T0 — all 32 at once.
    EXPECT_EQ(750000u % kSlotPitchUs, 0u);
    EXPECT_EQ(750000u / kSlotPitchUs, 16u);
    for (uint32_t k = 0; k + 16 < kSlotCount; ++k)
        EXPECT_EQ(t0ForSlot(0, 0, k) + 750000, t0ForSlot(0, 0, k + 16));
}
