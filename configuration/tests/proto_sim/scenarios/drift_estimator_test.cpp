// DriftEstimator — node-vs-hub clock drift from burst arrival times.
//
// The measurement is a straight line fitted through ~17 points spanning 1.4 s,
// where the signal at 20 ppm is ~28 µs and the noise is ~10 µs of interrupt
// jitter. That ratio is the whole reason this is a least-squares fit and not a
// subtraction of the first and last sample, and it is why these tests inject
// realistic jitter rather than clean data.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

#include "DriftEstimator.h"

using namespace drift;

namespace {

// Build one burst as the node would see it: copy i arrives at
// i * spacing * (1 + ppm/1e6), plus interrupt jitter.
std::vector<Sample> burst(int32_t ppm, uint8_t copies = 17,
                          int32_t jitter_us = 0, unsigned seed = 1) {
    std::srand(seed);
    std::vector<Sample> v;
    for (uint8_t i = 0; i < copies; i++) {
        const double nominal = double(int64_t(i) * kCopySpacingUs);
        int64_t t = int64_t(nominal * (1.0 + ppm / 1e6));
        if (jitter_us)
            t += (std::rand() % (2 * jitter_us + 1)) - jitter_us;
        v.push_back(Sample{t, i});
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// The measurement
// ---------------------------------------------------------------------------

TEST(DriftEstimator, PerfectClocksReadZero) {
    const auto b = burst(0);
    const auto r = estimate(b.data(), (uint8_t) b.size());
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.ppm, 0, 1);
}

TEST(DriftEstimator, RecoversAKnownDrift) {
    for (int32_t ppm : {-50, -20, -5, 5, 20, 50}) {
        const auto b = burst(ppm);
        const auto r = estimate(b.data(), (uint8_t) b.size());
        ASSERT_TRUE(r.valid) << ppm;
        EXPECT_NEAR(r.ppm, ppm, 2) << "injected " << ppm;
    }
}

TEST(DriftEstimator, SurvivesRealisticInterruptJitter) {
    // ~10 µs of jitter against a 28 µs signal. A single burst should still land
    // within a few ppm — this is the claim the whole design rests on.
    int worst = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
        const auto b = burst(20, 17, 10, seed);
        const auto r = estimate(b.data(), (uint8_t) b.size());
        ASSERT_TRUE(r.valid);
        worst = std::max(worst, std::abs(r.ppm - 20));
    }
    EXPECT_LT(worst, 15) << "one burst with 10 us jitter should resolve to ~single-digit ppm";
}

TEST(DriftEstimator, AveragingAcrossBurstsTightensIt) {
    // The reason Accumulator exists: one burst is noisy, ten are not.
    Accumulator acc;
    for (unsigned seed = 1; seed <= 10; seed++) {
        const auto b = burst(20, 17, 10, seed);
        acc.add(estimate(b.data(), (uint8_t) b.size()));
    }
    ASSERT_TRUE(acc.ready());
    EXPECT_NEAR(acc.mean_ppm(), 20, 5);
}

TEST(DriftEstimator, SignOfDriftIsUnambiguous) {
    // Getting this backwards would make a scheduled RX window open on the wrong
    // side of the expected instant — worse than not correcting at all.
    EXPECT_GT(estimate(burst(30).data(), 17).ppm, 0)
        << "a node clock running slow must report POSITIVE ppm";
    EXPECT_LT(estimate(burst(-30).data(), 17).ppm, 0);
}

// ---------------------------------------------------------------------------
// Refusing to measure what it cannot
// ---------------------------------------------------------------------------

TEST(DriftEstimator, TooFewSamplesIsInvalid) {
    const auto b = burst(20, 2);
    EXPECT_FALSE(estimate(b.data(), (uint8_t) b.size()).valid)
        << "two points always fit a line perfectly, including through noise";
}

TEST(DriftEstimator, NullIsInvalid) {
    EXPECT_FALSE(estimate(nullptr, 10).valid);
}

TEST(DriftEstimator, AllSamplesFromOneCopyIndexIsInvalid) {
    // Zero span: the fit would divide by zero. This is reachable in practice —
    // the node can hear the same copy index of several bursts and nothing else.
    Sample s[4] = {{0, 3}, {1500000, 3}, {3000000, 3}, {4500000, 3}};
    const auto r = estimate(s, 4);
    EXPECT_FALSE(r.valid) << "no baseline to fit over";
}

TEST(DriftEstimator, SpanIsReportedSoAShortFitCanBeRejected) {
    Sample s[3] = {{0, 0}, {88235, 1}, {176470, 2}};
    const auto r = estimate(s, 3);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.span_us, 2 * kCopySpacingUs)
        << "a caller must be able to see it only had a 0.18 s baseline";
}

TEST(DriftEstimator, GapsInTheBurstAreFine) {
    // The node will miss copies — it does not hear all 17. The fit must work on
    // whatever arrived, which is the point of regressing against burstIndex
    // rather than assuming consecutive samples.
    const auto full = burst(25);
    std::vector<Sample> sparse{full[0], full[4], full[9], full[16]};
    const auto r = estimate(sparse.data(), (uint8_t) sparse.size());
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.ppm, 25, 2);
    EXPECT_EQ(r.samples, 4);
}

// ---------------------------------------------------------------------------
// Constants and downstream use
// ---------------------------------------------------------------------------

TEST(DriftEstimator, CopySpacingIsNotRoundedTo88ms) {
    // 1500/17 = 88235 us. Using a round 88000 would itself inject 2670 ppm of
    // error — 130x the drift being measured.
    EXPECT_EQ(kCopySpacingUs, 1500000 / 17);
    EXPECT_EQ(kCopySpacingUs, 88235);
}

TEST(DriftEstimator, DriftOverASleepSizesTheGuardBand) {
    // This is what a scheduled RX window has to tolerate.
    EXPECT_EQ(drift_us_over(600LL * 1000000, 20), 12000);        // 10 min -> 12 ms
    EXPECT_EQ(drift_us_over(21600LL * 1000000, 20), 432000);     //  6 h   -> 432 ms
}

TEST(DriftEstimator, DriftOverASleepHandlesNegativePpm) {
    EXPECT_EQ(drift_us_over(600LL * 1000000, -20), -12000);
}

TEST(DriftEstimator, AccumulatorIgnoresInvalidFits) {
    Accumulator acc;
    Result bad{};                       // valid = false
    acc.add(bad);
    EXPECT_FALSE(acc.ready()) << "an unfittable burst must not be averaged in as 0 ppm";
    acc.add(estimate(burst(20).data(), 17));
    EXPECT_TRUE(acc.ready());
    EXPECT_NEAR(acc.mean_ppm(), 20, 2);
}

// ---------------------------------------------------------------------------
// Drift-test duration policy — the node decides, not the hub
// ---------------------------------------------------------------------------

TEST(DriftTestDuration, ZeroMeansTheNodeDefault) {
    EXPECT_EQ(testDurationS(0), kTestDefaultS);
}

TEST(DriftTestDuration, AReasonableRequestIsHonoured) {
    EXPECT_EQ(testDurationS(60), 60u);
    EXPECT_EQ(testDurationS(kTestMaxS), kTestMaxS);
}

TEST(DriftTestDuration, AnAbsurdRequestIsCapped) {
    // A test holds the radio in continuous RX at ~11 mA against a ~1.2 mA
    // interactive average. If a buggy or replayed hub frame could ask for
    // "forever", the pack would be flat before anyone noticed — so the ceiling
    // is the nodes, and it is not negotiable.
    EXPECT_EQ(testDurationS(kTestMaxS + 1), kTestMaxS);
    EXPECT_EQ(testDurationS(86400), kTestMaxS);
    EXPECT_EQ(testDurationS(0xFFFFFFFFu), kTestMaxS);
}

TEST(DriftTestDuration, TheCapIsShorterThanAnyPlausibleSleep) {
    // Sanity: the ceiling has to be small next to the battery it protects.
    EXPECT_LE(kTestMaxS, 3600u) << "an hour of continuous RX is already too much";
}
