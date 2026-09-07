// PhaseTracker — the bimodality guard.
//
// The design's B2 gate is "phaseErrUs inside +/-2 ms, on every node, over
// days" — and the failure it is really guarding against is not noise but
// SHAPE. A node that stamps its neighbours' frames produces two clusters one
// slot pitch apart whose mean sits innocently near zero. These tests assert
// that such a distribution is rejected.
//
// See docs/implementation-plan.md 4.6 and docs/test-plan.md 5.5.

#include <gtest/gtest.h>

#include "PhaseTracker.h"
#include "TimedGrid.h"

using namespace phase;

namespace {
constexpr uint32_t kGuard = timedgrid::kGuardUs;   // 14080

void feed(Stats &s, int32_t err_us) {
    commit(s, Sample{1000000 + err_us, 1000000}, kGuard);
}
}  // namespace

TEST(PhaseTracker, EmptyStatsAreNotTrusted) {
    Stats s;
    EXPECT_FALSE(s.valid());
    EXPECT_FALSE(phaseTrustworthy(s, kGuard));
    EXPECT_EQ(s.spread_us(), 0);
}

TEST(PhaseTracker, ATightDistributionIsTrusted) {
    Stats s;
    for (int i = 0; i < 20; ++i) feed(s, (i % 5) - 2);   // +/-2 us
    EXPECT_TRUE(phaseTrustworthy(s, kGuard));
    EXPECT_EQ(s.outside_guard, 0u);
    EXPECT_LE(s.spread_us(), 5);
}

TEST(PhaseTracker, ABimodalDistributionIsRejectedDespiteAnInnocentMean) {
    // THE test. Half the samples are this node's own frames, half are a
    // neighbour's one slot pitch away — the exact shape produced by stamping
    // before the address filter. The mean lands near half a pitch, but even a
    // symmetric version centred on zero must be rejected on SPREAD alone.
    Stats s;
    const int32_t pitch = (int32_t) timedgrid::kSlotPitchUs;
    for (int i = 0; i < 10; ++i) { feed(s, -pitch / 2); feed(s, +pitch / 2); }

    EXPECT_EQ(s.mean_us(), 0) << "the mean says nothing is wrong";
    // The pitch is odd (46875), so +/-pitch/2 truncates to a spread one
    // microsecond short of it. Asserting the shape, not a rounding artefact.
    EXPECT_NEAR(s.spread_us(), pitch, 2) << "the spread says everything is wrong";
    EXPECT_GT((uint32_t) s.spread_us(), kGuard * 3);
    EXPECT_FALSE(phaseTrustworthy(s, kGuard))
        << "a bimodal distribution must never be trusted for a timed window";
}

TEST(PhaseTracker, SamplesOutsideTheGuardAreCountedAndDisqualify) {
    Stats s;
    for (int i = 0; i < 20; ++i) feed(s, 0);
    EXPECT_TRUE(phaseTrustworthy(s, kGuard));

    feed(s, (int32_t) kGuard + 1);
    EXPECT_EQ(s.outside_guard, 1u);
    EXPECT_FALSE(phaseTrustworthy(s, kGuard))
        << "one frame already outside the window disqualifies the node";
}

TEST(PhaseTracker, GuardEdgesAreInclusive) {
    Stats s;
    feed(s, (int32_t) kGuard);
    feed(s, -(int32_t) kGuard);
    EXPECT_EQ(s.outside_guard, 0u) << "exactly at the edge is still caught";
}

TEST(PhaseTracker, TooFewSamplesIsNotTrustedHoweverGoodTheyLook) {
    // Promotion needs a long baseline; demotion is immediate. Asymmetry by
    // design — a handful of perfect samples is not evidence.
    Stats s;
    for (int i = 0; i < 7; ++i) feed(s, 0);
    EXPECT_FALSE(phaseTrustworthy(s, kGuard, /*min_samples=*/8));
    feed(s, 0);
    EXPECT_TRUE(phaseTrustworthy(s, kGuard, 8));
}

TEST(PhaseTracker, AbsurdExpectationsClampInsteadOfWrapping) {
    // An uninitialised expectation, or a frame from before the anchor, must not
    // wrap a 64-bit difference into a small plausible-looking 32-bit error.
    Stats s;
    commit(s, Sample{/*measured=*/(int64_t) INT32_MAX * 8, /*expected=*/0}, kGuard);
    EXPECT_EQ(s.last_us, INT32_MAX);
    EXPECT_EQ(s.outside_guard, 1u);

    Stats t;
    commit(t, Sample{0, (int64_t) INT32_MAX * 8}, kGuard);
    EXPECT_EQ(t.last_us, INT32_MIN);
    EXPECT_EQ(t.outside_guard, 1u);
}

TEST(PhaseTracker, T0RecoveryMatchesTheSharedDefinition) {
    // Never open-coded: same one-line recovery as LoraTiming.
    const int64_t rxdone = 5000000;
    EXPECT_EQ(t0FromRx(rxdone, 60), loratiming::t0FromRxDoneUs(rxdone, 60));
    EXPECT_EQ(t0FromRx(rxdone, 60), rxdone - 38912);
}

TEST(PhaseTracker, ResetClearsEverything) {
    Stats s;
    for (int i = 0; i < 5; ++i) feed(s, 100);
    s.reset();
    EXPECT_EQ(s.n, 0u);
    EXPECT_EQ(s.outside_guard, 0u);
    EXPECT_FALSE(s.valid());
}

TEST(PhaseTracker, RtcSourceGatesModeBAndUnknownIsNotTheCrystal) {
    // A zeroed field must mean "unknown", never "crystal": the safe reading of
    // a node that has not reported is that it cannot hold phase.
    EXPECT_EQ(static_cast<uint8_t>(RtcSlowSrc::Unknown), 0);
    EXPECT_NE(RtcSlowSrc::Unknown, RtcSlowSrc::Crystal);
    EXPECT_NE(RtcSlowSrc::InternalRc, RtcSlowSrc::Crystal);
}
