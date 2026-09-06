// Mode A reception geometry.
//
// Mode A is not new, and that is exactly why it needs tests: it is what every
// other mode falls back TO, so a regression in it is a fleet-wide outage with
// no safety net underneath.
//
// The quantity is P(at least one of 17 burst copies lands in a receive window),
// computed two independent ways — an interval merge in closed form, and a brute
// enumeration over every microsecond of the window period. They must agree. The
// closed form alone would be self-consistent and could still be wrong; the
// enumeration alone is too slow to parameterise.
//
// The headline is that treating the windows as independent gives 69.7 % and is
// wrong by 27 points. That error is not arithmetic — the closed form was right
// and the intuition was wrong — which is the class of mistake only a test that
// computes the same thing twice can catch.
//
// See docs/test-plan.md section 4.1 and implementation-plan.md section 3.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "LoraTiming.h"

using namespace loratiming;

namespace {

// The window period: how often the node opens a window. Mode A opens three per
// 1500 ms round, i.e. one every 500 ms; the single-window mode opens one.
constexpr uint32_t kThreeWindowPeriodUs = 500000;
constexpr uint32_t kOneWindowPeriodUs   = 1500000;

// Copy i leaves at i * stride; it is caught iff (x + i*stride) mod P lies in
// [0, w), where x is the unknown phase of the burst against the window grid.
// The answer is the measure of the union of those 17 residue intervals.
uint64_t unionClosedForm(uint32_t period_us, uint32_t window_us,
                         uint8_t copies = kBurstCopies,
                         uint32_t stride_us = kBurstCopyStrideUs) {
    std::vector<std::pair<int64_t, int64_t>> segs;
    for (uint8_t i = 0; i < copies; ++i) {
        const int64_t r = (int64_t) ((uint64_t) i * stride_us % period_us);
        if (r + window_us <= period_us) {
            segs.push_back({r, r + window_us});
        } else {                       // wraps the period boundary
            segs.push_back({r, (int64_t) period_us});
            segs.push_back({0, r + window_us - (int64_t) period_us});
        }
    }
    std::sort(segs.begin(), segs.end());
    uint64_t total = 0;
    int64_t cs = segs[0].first, ce = segs[0].second;
    for (size_t i = 1; i < segs.size(); ++i) {
        if (segs[i].first <= ce) ce = std::max(ce, segs[i].second);
        else { total += (uint64_t) (ce - cs); cs = segs[i].first; ce = segs[i].second; }
    }
    return total + (uint64_t) (ce - cs);
}

// The same measure, by asking the question directly at every microsecond.
uint64_t unionEnumerated(uint32_t period_us, uint32_t window_us,
                         uint8_t copies = kBurstCopies,
                         uint32_t stride_us = kBurstCopyStrideUs) {
    uint64_t hits = 0;
    for (uint32_t x = 0; x < period_us; ++x) {
        for (uint8_t i = 0; i < copies; ++i) {
            if (((uint64_t) x + (uint64_t) i * stride_us) % period_us < window_us) {
                ++hits;
                break;
            }
        }
    }
    return hits;
}

double frac(uint64_t n, uint32_t d) { return (double) n / (double) d; }

// The window width, rounded DOWN to whole milliseconds. The real window is
// 29.44 ms; using 29 is the conservative choice, and the ONLY rule that matters
// is that the predicate, the union and the quoted table all use the same value.
constexpr uint32_t kW = 29000;

}  // namespace

// ---------------------------------------------------------------------------
// The two computations must agree
// ---------------------------------------------------------------------------

TEST(ModeAGeometry, ClosedFormMatchesEnumeration) {
    for (uint32_t w : {29000u, 29440u}) {
        EXPECT_EQ(unionClosedForm(kThreeWindowPeriodUs, w),
                  unionEnumerated(kThreeWindowPeriodUs, w)) << "w=" << w;
        EXPECT_EQ(unionClosedForm(kOneWindowPeriodUs, w),
                  unionEnumerated(kOneWindowPeriodUs, w)) << "w=" << w;
    }
}

TEST(ModeAGeometry, AgreeForConfinedBurstsToo) {
    // The confined-region variants of implementation-plan.md section 4.5.
    EXPECT_EQ(unionClosedForm(kThreeWindowPeriodUs, kW, 3, 150000),
              unionEnumerated(kThreeWindowPeriodUs, kW, 3, 150000));
    EXPECT_EQ(unionClosedForm(kThreeWindowPeriodUs, kW, 5, 100000),
              unionEnumerated(kThreeWindowPeriodUs, kW, 5, 100000));
}

// ---------------------------------------------------------------------------
// The pinned table
// ---------------------------------------------------------------------------

TEST(ModeAGeometry, ThreeWindowsPerRound) {
    const uint64_t u = unionClosedForm(kThreeWindowPeriodUs, kW);
    EXPECT_EQ(u, 482000u);                                  // of 500 000 us
    EXPECT_NEAR(frac(u, kThreeWindowPeriodUs), 0.9640, 1e-4);
}

TEST(ModeAGeometry, OneWindowUnsynchronised) {
    const uint64_t u = unionClosedForm(kOneWindowPeriodUs, kW);
    EXPECT_EQ(u, 493000u);                                  // of 1 500 000 us
    EXPECT_NEAR(frac(u, kOneWindowPeriodUs), 0.3287, 1e-4);
}

TEST(ModeAGeometry, ResidueGapStructure) {
    // The residues sort to gaps of 28 ms (x11) and 32 ms (x6). That structure is
    // WHY the answer is 96.4 %: eleven of the seventeen windows are clipped by
    // the next copy, six are not. 11*28 + 6*29 = 482.
    std::vector<uint32_t> r;
    for (uint8_t i = 0; i < kBurstCopies; ++i)
        r.push_back((uint32_t) ((uint64_t) i * kBurstCopyStrideUs % kThreeWindowPeriodUs));
    std::sort(r.begin(), r.end());

    int g28 = 0, g32 = 0;
    for (size_t i = 0; i < r.size(); ++i) {
        const uint32_t gap =
            (r[(i + 1) % r.size()] + kThreeWindowPeriodUs - r[i]) % kThreeWindowPeriodUs;
        if (gap == 28000) ++g28;
        else if (gap == 32000) ++g32;
        else ADD_FAILURE() << "unexpected gap " << gap;
    }
    EXPECT_EQ(g28, 11);
    EXPECT_EQ(g32, 6);
    EXPECT_EQ(11u * 28000 + 6u * 29000, 482000u);
}

TEST(ModeAGeometry, IndependenceTrapIsWrong) {
    // Treating the three windows as independent Bernoulli trials gives 69.7 %.
    // The copies are strongly ANTI-correlated: 88 ms against 500 ms sweeps them
    // almost uniformly across the window period, which is precisely the
    // "tuned to the window scheme" property the burst relies on.
    const double p_one = frac(unionClosedForm(kOneWindowPeriodUs, kW), kOneWindowPeriodUs);
    const double naive = 1.0 - (1.0 - p_one) * (1.0 - p_one) * (1.0 - p_one);
    EXPECT_NEAR(naive, 0.6974, 1e-4);

    const double truth = frac(unionClosedForm(kThreeWindowPeriodUs, kW),
                              kThreeWindowPeriodUs);
    EXPECT_GT(truth - naive, 0.26) << "the independence assumption understates by "
                                      "~27 points and must not be reintroduced";
}

TEST(ModeAGeometry, WindowWidthIsAParameterNotAConstant) {
    // The predicate, the union and any quoted table must use ONE w. At 29.44 ms
    // the same routine gives 96.93 % and 33.37 % — both defensible, and mixing
    // them across two tables in one document is how the numbers stop adding up.
    EXPECT_NEAR(frac(unionClosedForm(kThreeWindowPeriodUs, 29440), kThreeWindowPeriodUs),
                0.9693, 1e-4);
    EXPECT_NEAR(frac(unionClosedForm(kOneWindowPeriodUs, 29440), kOneWindowPeriodUs),
                0.3337, 1e-4);
}

// ---------------------------------------------------------------------------
// Why a reserved contention region was rejected
// ---------------------------------------------------------------------------

TEST(ModeAGeometry, ConfiningCopiesDestroysTheSweep) {
    // Kept as a REGRESSION test: a ~300 ms reserved region for unsynced nodes
    // was proposed and killed by this arithmetic. Confining the copies removes
    // the incommensurate sweep that makes the burst work at all.
    const double p3 = frac(unionClosedForm(kThreeWindowPeriodUs, kW, 3, 150000),
                           kThreeWindowPeriodUs);
    const double p5 = frac(unionClosedForm(kThreeWindowPeriodUs, kW, 5, 100000),
                           kThreeWindowPeriodUs);
    const double p17 = frac(unionClosedForm(kThreeWindowPeriodUs, kW),
                            kThreeWindowPeriodUs);

    EXPECT_NEAR(p3, 0.1740, 1e-4);   // 3 copies in 300 ms
    EXPECT_NEAR(p5, 0.2900, 1e-4);   // 5 copies in 400 ms
    EXPECT_GT(p17, 5.0 * p3);
}
