// LoraTiming — the reference point, pinned by hand.
//
// Nothing else in the system can check these numbers: every other timing
// assertion is expressed in terms of them, so an error here is invisible
// everywhere and wrong everywhere. That is why this file restates the constants
// as literals rather than recomputing them from the header's own expressions —
// a test that derives its expectation the same way the code does proves only
// that the code is self-consistent.
//
// See docs/test-plan.md section 5.1.

#include <gtest/gtest.h>

#include "LoraTiming.h"

using namespace loratiming;

// ---------------------------------------------------------------------------
// The frame anatomy
// ---------------------------------------------------------------------------

TEST(LoraTiming, SymbolTime) {
    // T_sym = 2^7 / 500000 = 256.0 us, and it must be EXACT — every other
    // constant is an integer multiple of it, so a fractional symbol time would
    // accumulate into the guard band.
    EXPECT_EQ(kSymbolUs, 256u);
}

TEST(LoraTiming, PreambleToT0) {
    // (n_pre + 4.25) * T_sym = 12.25 * 256 = 3136 us.
    EXPECT_EQ(kPreambleToT0Us, 3136u);
}

TEST(LoraTiming, HeaderIsEightSymbols) {
    EXPECT_EQ(kHeaderSymbols, 8u);
    EXPECT_EQ(kHeaderUs, 2048u);
}

// ---------------------------------------------------------------------------
// The airtime table of implementation-plan.md section 2.1
// ---------------------------------------------------------------------------

TEST(LoraTiming, NsymPinned) {
    EXPECT_EQ(symbolCount(25), 72u);    // ack
    EXPECT_EQ(symbolCount(45), 120u);   // beacon
    EXPECT_EQ(symbolCount(60), 152u);   // routine command
    EXPECT_EQ(symbolCount(152), 360u);  // 8-entry ScheduleConfig
}

TEST(LoraTiming, TimeOnAirPinned) {
    EXPECT_EQ(timeOnAirUs(25), 21568u);
    EXPECT_EQ(timeOnAirUs(45), 33856u);
    EXPECT_EQ(timeOnAirUs(60), 42048u);
    EXPECT_EQ(timeOnAirUs(152), 95296u);
}

TEST(LoraTiming, TimeOnAirIsPreamblePlusT0ToRxDone) {
    for (uint32_t len : {0u, 1u, 25u, 45u, 60u, 152u, 255u})
        EXPECT_EQ(timeOnAirUs(len), kPreambleToT0Us + t0ToRxDoneUs(len)) << len;
}

TEST(LoraTiming, AirtimeIsMonotonicInPayload) {
    // Not a tautology: the ceil term steps, so this catches a sign error in the
    // numerator that would otherwise only show at one payload length.
    for (uint32_t len = 1; len <= 255; ++len)
        EXPECT_GE(timeOnAirUs(len), timeOnAirUs(len - 1)) << len;
}

// ---------------------------------------------------------------------------
// The convention that will bite
// ---------------------------------------------------------------------------

TEST(LoraTiming, PayloadLengthConventionIsRegRxNbBytes) {
    // PL EXCLUDES the CRC; the +16*CRC term in the formula accounts for it.
    //
    // This is the most valuable assertion in the file. The ceil steps in blocks
    // of (CR+4) = 8 symbols per 28 bits, so a two-byte error in PL is worth
    // ZERO or 2048 us depending on where it falls relative to a block boundary.
    // A constant bias would be calibrated away in a day; a bias that appears and
    // disappears with payload length looks exactly like an intermittent crystal
    // fault, and would be chased in the wrong subsystem.
    EXPECT_EQ(symbolCount(58), 152u);
    EXPECT_EQ(symbolCount(60), 152u);   // same block: a 2-byte error costs 0
    EXPECT_EQ(symbolCount(62), 160u);   // next block: the same error costs 2048 us

    EXPECT_EQ(t0ToRxDoneUs(62) - t0ToRxDoneUs(60), 2048u);
    EXPECT_EQ(t0ToRxDoneUs(60) - t0ToRxDoneUs(58), 0u);
}

TEST(LoraTiming, BlockBoundariesStepByExactlyOneCodingBlock) {
    // Wherever it steps, it steps by (CR+4) symbols and never by anything else.
    for (uint32_t len = 1; len <= 255; ++len) {
        const uint32_t d = symbolCount(len) - symbolCount(len - 1);
        EXPECT_TRUE(d == 0 || d == kCodingRateDenom)
            << "len " << len << " stepped by " << d;
    }
}

// ---------------------------------------------------------------------------
// T0 recovery
// ---------------------------------------------------------------------------

TEST(LoraTiming, T0FromRxDoneIsOneLine) {
    // The node computes T0 = t_rxdone - n_sym(len)*T_sym in ONE operation.
    //
    // Decomposing it as "subtract the payload time, then the header time" is
    // arithmetically identical ONLY if the payload term already excludes the
    // header. It is easy to write the version that does not, and this test
    // makes that mistake a failure rather than a review finding: the wrong
    // decomposition lands exactly 2048 us early.
    const int64_t t_rxdone = 1'000'000'000;
    const uint32_t len = 60;

    const int64_t correct = t0FromRxDoneUs(t_rxdone, len);
    EXPECT_EQ(correct, t_rxdone - 38912);   // 152 symbols * 256 us

    const int64_t payload_symbols = symbolCount(len) - kHeaderSymbols;
    const int64_t double_counted =
        t_rxdone - payload_symbols * kSymbolUs - kHeaderUs - kHeaderUs;
    EXPECT_EQ(correct - double_counted, (int64_t) kHeaderUs);
    EXPECT_EQ(correct - double_counted, 2048);
}

TEST(LoraTiming, T0RecoveryRoundTripsAgainstTheHubsFireInstant) {
    // The two ends name the same T0 from opposite directions. With a known ramp
    // the round trip must be exact, or the grid anchor and the node's phase
    // measurement are in different units.
    const int64_t t0 = 500'000'000;
    const uint32_t ramp = 220;
    const uint32_t len = 60;

    const int64_t fire = fireInstantUs(t0, ramp);
    EXPECT_EQ(fire, t0 - 3136 - 220);

    const int64_t air_start = fire + ramp;
    const int64_t rxdone = air_start + timeOnAirUs(len);
    EXPECT_EQ(t0FromRxDoneUs(rxdone, len), t0);
}

// ---------------------------------------------------------------------------
// The ruler
// ---------------------------------------------------------------------------

TEST(LoraTiming, BurstStrideIs88000) {
    // NOT 1500/17 = 88235. DriftEstimator.h records that reading the stride as
    // 88.235 ms rather than the hub's integer 88.000 produced a -2663 ppm error
    // that survived three firmware revisions. One definition, both ends.
    EXPECT_EQ(kBurstCopyStrideUs, 88000u);
    EXPECT_EQ(kBurstCopies, 17u);
    EXPECT_NE(kBurstCopyStrideUs, 1500000u / 17u);
}

TEST(LoraTiming, GridPeriodsAreIntegerMilliseconds) {
    EXPECT_EQ(kBurstCopyStrideUs % 1000, 0u);
}

TEST(LoraTiming, BurstOccupies1450msForARoutineCommand) {
    // 16 * 88 ms + 42.048 ms. Quoted throughout the plan; pinned once here.
    const uint32_t span = (kBurstCopies - 1) * kBurstCopyStrideUs + timeOnAirUs(60);
    EXPECT_EQ(span, 1450048u);
    EXPECT_GT(span, 1'400'000u);
}

// ---------------------------------------------------------------------------
// CAD, and the lead in front of an aimed uplink
// ---------------------------------------------------------------------------

TEST(LoraTiming, CadIsOneSymbolPlus32Chips) {
    // Derived, not assumed: the datasheet's own decomposition is one symbol of
    // listening (2^SF / BW) plus 32/BW of correlation. Both exact once SF and
    // BW are fixed — unlike kDetectSymbolsAssumed, which is a rule of thumb.
    EXPECT_EQ(kCadUs, 320u);
    EXPECT_EQ(kCadUs, (1u << kSpreadingFactor) * 1000000u / kBandwidthHz
                    + 32u * 1000000u / kBandwidthHz);
}

TEST(LoraTiming, CadStartIsTheFireInstantMinusTheCad) {
    // The node transmits CAD-first, so a frame aimed at an instant must begin
    // its CAD a whole CAD earlier or it arrives 320 us late — which is small,
    // and exactly the kind of small that accumulates into a missed window when
    // it is left out of the arithmetic entirely.
    const int64_t t0 = 5'000'000;
    EXPECT_EQ(cadStartInstantUs(t0, 0, 0), fireInstantUs(t0, 0) - (int64_t) kCadUs);
    EXPECT_EQ(t0 - cadStartInstantUs(t0, 0, 0),
              (int64_t) kPreambleToT0Us + (int64_t) kCadUs);
}

TEST(LoraTiming, UnmeasuredLeadsMakeTheUplinkLateNotEarly) {
    // Both unmeasured terms are passed as zero, and the sign of that choice is
    // the point: a zero lead fires LATE by exactly the term it omitted, which
    // is a knowable error inside a 14 080 us guard. Guessing a value would put
    // the error on the early side of the window AND make it unknowable.
    const int64_t t0 = 5'000'000;
    EXPECT_LT(cadStartInstantUs(t0, 3000, 200), cadStartInstantUs(t0, 0, 0));
    EXPECT_EQ(cadStartInstantUs(t0, 0, 0) - cadStartInstantUs(t0, 3000, 200), 3200);
}
