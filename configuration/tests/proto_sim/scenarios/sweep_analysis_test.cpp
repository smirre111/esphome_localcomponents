// SweepAnalysis — recovering T_detect from an arm-offset sweep (HW-2).
//
// The valuable test is a ROUND TRIP: sim/air_channel.h is told a detection
// time, produces hits and misses from its own physics, and the analysis must
// recover the number it was never given. Nothing in analyse() may assume the
// design's 5-symbol rule of thumb, or the measurement would be circular — and
// the whole reason HW-2 exists is that the rule of thumb is unverified.
//
// See docs/test-plan.md section 10.6 HW-2.

#include <gtest/gtest.h>

#include <vector>

#include "SweepAnalysis.h"
#include "sim/air_channel.h"

using namespace sweep;

namespace {

// Run one arm-offset sweep against the simulated radio, with a detection time
// the analysis is not told.
std::vector<Point> simulateSweep(uint32_t true_detect_us, uint32_t window_us,
                                 int32_t step_us = 500, uint32_t marks = 20) {
    std::vector<Point> pts;
    const uint32_t arm_lead = loratiming::kPreambleToT0Us + (window_us - true_detect_us) / 2;

    for (int32_t off = -(int32_t) window_us; off <= (int32_t) window_us; off += step_us) {
        Point p;
        p.offset_us = off;
        for (uint32_t m = 0; m < marks; ++m) {
            const int64_t t0 = 1000000 + (int64_t) m * timedgrid::kRoundUs;
            proto_sim::RxWindow w{t0 - (int64_t) arm_lead + off, (int64_t) window_us};
            proto_sim::Transmission t; t.t0_us = t0; t.payload_len = 60;
            p.armed++;
            if (proto_sim::caught(w, t, true_detect_us)) p.hit++;
        }
        pts.push_back(p);
    }
    return pts;
}

}  // namespace

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(SweepAnalysis, RecoversTheDetectionTimeItWasNeverTold) {
    const uint32_t W = timedgrid::kWindowUs;
    for (uint32_t truth : {512u, 1280u, 2560u, 5120u}) {
        const auto pts = simulateSweep(truth, W);
        const auto r = analyse(pts.data(), (uint32_t) pts.size(), W);

        ASSERT_TRUE(r.valid) << "truth " << truth;
        // Recovered to within one sweep step, which is the resolution the
        // method can possibly have.
        EXPECT_NEAR(r.detect_us, (int32_t) truth, 1000) << "truth " << truth;
    }
}

TEST(SweepAnalysis, TheTwoEdgesAreSymmetricAboutZero) {
    // If they are not, the window is not where the node thinks it is, and the
    // number to fix is the arm lead rather than T_detect.
    const auto pts = simulateSweep(1280, timedgrid::kWindowUs);
    const auto r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    ASSERT_TRUE(r.valid);
    EXPECT_LE(std::abs(r.asymmetry_us), 1000)
        << "early " << r.early_edge_us << " late " << r.late_edge_us;
}

TEST(SweepAnalysis, SpanIsTwiceTheGuardBand) {
    const auto pts = simulateSweep(timedgrid::kDetectUs, timedgrid::kWindowUs);
    const auto r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.span_us, 2 * (int32_t) timedgrid::kGuardUs, 1000);
}

TEST(SweepAnalysis, AgreementWithTheAssumptionIsReportedNotAssumed) {
    // At the assumed value it agrees; at four times it does not, and the design
    // would need its guard band revisited.
    auto pts = simulateSweep(timedgrid::kDetectUs, timedgrid::kWindowUs);
    auto r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    EXPECT_TRUE(agreesWithAssumption(r));

    pts = simulateSweep(4 * timedgrid::kDetectUs, timedgrid::kWindowUs);
    r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    ASSERT_TRUE(r.valid);
    EXPECT_FALSE(agreesWithAssumption(r))
        << "a four-fold error must not pass as agreement";
}

// ---------------------------------------------------------------------------
// Refusals — a sweep that measured nothing must not look like a result
// ---------------------------------------------------------------------------

TEST(SweepAnalysis, NoPointsIsNotAResult) {
    EXPECT_FALSE(analyse(nullptr, 0, timedgrid::kWindowUs).valid);
    Point none[1] = {};
    EXPECT_FALSE(analyse(none, 1, timedgrid::kWindowUs).valid);
}

TEST(SweepAnalysis, NothingReceivedAnywhereIsAFailureNotAZero) {
    // A dead radio would otherwise report T_detect == W, which is a number and
    // would be believed.
    std::vector<Point> pts;
    for (int32_t off = -20000; off <= 20000; off += 500)
        pts.push_back(Point{off, 20, 0});
    const auto r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.receiving_points, 0u);
}

TEST(SweepAnalysis, ASpanWiderThanTheWindowIsRefused) {
    // Physically impossible: it means the sweep is measuring something else — a
    // second frame arriving, or a window that never closed. Reporting a
    // negative detection time as a result would be worse than reporting none.
    std::vector<Point> pts;
    for (int32_t off = -40000; off <= 40000; off += 500)
        pts.push_back(Point{off, 20, 20});
    const auto r = analyse(pts.data(), (uint32_t) pts.size(), timedgrid::kWindowUs);
    EXPECT_FALSE(r.valid);
}

TEST(SweepAnalysis, TooFewMarksAtAnOffsetDoesNotCount) {
    // Right at an edge the answer is genuinely marginal; one lucky frame must
    // not widen the measured span.
    Point p{0, 3, 3};
    EXPECT_FALSE(p.receives()) << "three marks is not evidence";
    Point q{0, 20, 19};
    EXPECT_TRUE(q.receives());
    Point marginal{0, 20, 10};
    EXPECT_FALSE(marginal.receives()) << "50 % is the edge, not reception";
}

TEST(SweepAnalysis, AWiderWindowMovesTheEdgesNotTheDetectionTime) {
    // symTimeout is a runtime register write, so a stale-sync fallback widens
    // the window. T_detect is a property of the radio and must come out the
    // same.
    const uint32_t wide = 200 * loratiming::kSymbolUs;
    const auto narrow_pts = simulateSweep(1280, timedgrid::kWindowUs);
    const auto wide_pts   = simulateSweep(1280, wide);

    const auto a = analyse(narrow_pts.data(), (uint32_t) narrow_pts.size(),
                           timedgrid::kWindowUs);
    const auto b = analyse(wide_pts.data(), (uint32_t) wide_pts.size(), wide);
    ASSERT_TRUE(a.valid);
    ASSERT_TRUE(b.valid);

    EXPECT_GT(b.span_us, a.span_us) << "a wider window has a wider span";
    EXPECT_NEAR(b.detect_us, a.detect_us, 1000)
        << "but the same detection time";
}
