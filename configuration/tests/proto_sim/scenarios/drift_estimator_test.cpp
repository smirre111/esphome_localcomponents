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

TEST(DriftEstimator, CopySpacingMirrorsTheHubsIntegerArithmetic) {
    // The hub does `roundDurationMs / txSlotsPerRound` in INTEGER ms:
    // 1500/17 = 88, then delays 88 ms. The mathematically exact 88235 us is
    // NOT what the radio does.
    //
    // An earlier version of this test asserted 88235 and called 88000 an
    // error. That was backwards, and it cost three firmware revisions: the
    // node reported -2156 and -2597 ppm for a crystal that cannot exceed
    // about +-20 ppm, which is exactly 88000/88235 - 1 = -2663 ppm.
    EXPECT_EQ(kCopySpacingUs, 88000);
    EXPECT_EQ(kCopySpacingUs, (kRoundUs / 1000 / kTxSlots) * 1000);
}

TEST(DriftEstimator, AWrongSpacingConstantLooksExactlyLikeDrift) {
    // Why the constant is dangerous: feed PERFECT copies spaced at the old
    // 88235 us and the estimator reports a large bogus drift, because the
    // ruler disagrees with reality. Nothing about the output says "your
    // constant is wrong" -- it just looks like a bad crystal.
    std::vector<Sample> v;
    for (uint8_t i = 0; i < 17; i++) v.push_back(Sample{int64_t(i) * 88235, i});
    const auto r = estimate(v.data(), (uint8_t) v.size());
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.ppm, 2670, 30) << "a 235 us/copy ruler error reads as ~2670 ppm";
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

// ---------------------------------------------------------------------------
// Minimum baseline — short fits are noise, not weak evidence
// ---------------------------------------------------------------------------

TEST(DriftUsable, AFullBurstIsUsable) {
    EXPECT_TRUE(usable(estimate(burst(20).data(), 17)));
}

TEST(DriftUsable, AThreeCopyFragmentIsRejected) {
    // Taken from a real run: burst grouping fragmented, producing 3-copy fits
    // spanning 176 ms that read -38584 ppm while 12-copy fits of the SAME
    // signal read about -18000. Averaging those in moves the answer away from
    // the truth.
    Sample s[3] = {{0, 0}, {88235, 1}, {176470, 2}};
    const auto r = estimate(s, 3);
    ASSERT_TRUE(r.valid) << "still a valid fit ...";
    EXPECT_FALSE(usable(r)) << "... but not one worth averaging in";
}

TEST(DriftUsable, TheThresholdIsHalfABurst) {
    EXPECT_EQ(kMinSpanUs, 8 * kCopySpacingUs);
    // Exactly at the threshold is accepted; one copy short is not.
    std::vector<Sample> at;
    for (uint8_t i = 0; i <= 8; i++) at.push_back(Sample{int64_t(i) * kCopySpacingUs, i});
    EXPECT_TRUE(usable(estimate(at.data(), (uint8_t) at.size())));
    at.pop_back();
    EXPECT_FALSE(usable(estimate(at.data(), (uint8_t) at.size())));
}

TEST(DriftUsable, AnInvalidFitIsNeverUsable) {
    Result bad{};
    EXPECT_FALSE(usable(bad));
}

// ---------------------------------------------------------------------------
// Measured spacing — the diagnostic that makes the ruler checkable
// ---------------------------------------------------------------------------

TEST(MeasuredSpacing, MatchesNominalForPerfectClocks) {
    const auto r = estimate(burst(0).data(), 17);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.measured_spacing_us, kCopySpacingUs, 2);
}

TEST(MeasuredSpacing, RevealsAMismatchedConstant) {
    // The case that actually happened: the hub emits at 88235 while the node
    // believes 88000. The ppm figure is misleading, but measured_spacing_us
    // states the truth plainly.
    std::vector<Sample> v;
    for (uint8_t i = 0; i < 17; i++) v.push_back(Sample{int64_t(i) * 88235, i});
    const auto r = estimate(v.data(), (uint8_t) v.size());
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.measured_spacing_us, 88235, 2)
        << "reports what the hub ACTUALLY did, independent of the constant";
}

TEST(MeasuredSpacing, TracksARealDrift) {
    const auto r = estimate(burst(1000).data(), 17);   // exaggerated for clarity
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.measured_spacing_us, kCopySpacingUs + kCopySpacingUs / 1000, 2);
}

// ---------------------------------------------------------------------------
// LongFit — the long-baseline fit that replaces per-burst estimation
// ---------------------------------------------------------------------------

namespace {
// A timing run: `count` frames at `period_us`, node clock off by ppm, with
// `jitter_us` of placement error on each frame.
LongFit run(int32_t ppm, uint32_t count, int64_t period_us,
            int32_t jitter_us = 0, unsigned seed = 1) {
    std::srand(seed);
    LongFit f;
    for (uint32_t i = 0; i < count; i++) {
        const int64_t nominal = (int64_t) i * period_us;
        int64_t rx = (int64_t) ((double) nominal * (1.0 + ppm / 1e6));
        if (jitter_us)
            rx += (std::rand() % (2 * jitter_us + 1)) - jitter_us;
        f.add(nominal, rx);
    }
    return f;
}
}  // namespace

TEST(LongFit, RecoversDriftFromACleanRun) {
    for (int32_t ppm : {-40, -20, -5, 5, 20, 40}) {
        const auto f = run(ppm, 300, 1000000);
        ASSERT_TRUE(f.ready());
        EXPECT_NEAR(f.ppm(), ppm, 1) << "injected " << ppm;
    }
}

TEST(LongFit, ALongBaselineDefeatsMillisecondJitter) {
    // THE point of this class. 1 ms of placement error per frame -- the hub's
    // FreeRTOS tick quantisation -- is ~700 ppm over a 1.4 s burst and wrecked
    // every per-burst measurement taken on hardware. Over a 300 s run the same
    // error must land within a few ppm.
    int worst = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
        const auto f = run(20, 300, 1000000, 1000, seed);
        worst = std::max(worst, std::abs(f.ppm() - 20));
    }
    EXPECT_LT(worst, 5) << "300 s of baseline should absorb 1 ms of per-frame jitter";
}

TEST(LongFit, AShortRunCannotResolveTheSameJitter) {
    // The contrast that justifies the change: identical jitter, 17 frames over
    // 1.4 s, is nowhere near good enough. This is what the old code did.
    int worst = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
        const auto f = run(20, 17, 88000, 1000, seed);
        worst = std::max(worst, std::abs(f.ppm() - 20));
    }
    EXPECT_GT(worst, 100) << "a 1.4 s baseline cannot resolve 20 ppm through 1 ms jitter";
}

TEST(LongFit, RefusesFewerThanThreeSamples) {
    LongFit f;
    f.add(0, 0);
    f.add(1000000, 1000000);
    EXPECT_FALSE(f.ready());
    EXPECT_EQ(f.ppm(), 0);
}

TEST(LongFit, AllSamplesAtOneNominalIsRefused) {
    LongFit f;
    for (int i = 0; i < 5; i++) f.add(0, i * 1000);
    EXPECT_EQ(f.ppm(), 0) << "zero span: no baseline to fit over";
}

TEST(LongFit, MeasuredPeriodExposesAWrongRuler) {
    // The hub emits every 1000500 us while the node believes 1000000. The ppm
    // figure is then wrong by 500 ppm, and measured_period_us is what says so.
    LongFit f;
    for (uint32_t i = 0; i < 100; i++) f.add((int64_t) i * 1000000, (int64_t) i * 1000500);
    EXPECT_NEAR(f.ppm(), 500, 1);
    EXPECT_NEAR(f.measured_period_us(99), 1000500, 2);
}

TEST(LongFit, SpanReportsTheRealBaseline) {
    const auto f = run(0, 300, 1000000);
    EXPECT_NEAR((double) f.span_us(), 299.0 * 1000000.0, 1000.0);
}

TEST(LongFit, ResetClearsEverything) {
    auto f = run(20, 300, 1000000);
    f.reset();
    EXPECT_FALSE(f.ready());
    EXPECT_EQ(f.ppm(), 0);
    EXPECT_EQ(f.span_us(), 0);
}
