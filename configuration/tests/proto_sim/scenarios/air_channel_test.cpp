// AirChannel — reception geometry measured through a radio that can miss.
//
// Until now the Mode A figures were a closed form checked against an
// enumeration of the SAME formula. This runs the frames past a receiver that
// opens and closes, so the geometry is measured rather than restated.
//
// See docs/mac-layer.md section 6 and docs/test-plan.md section 4.1.

#include <gtest/gtest.h>

#include "sim/air_channel.h"

using namespace proto_sim;

namespace {
constexpr int64_t kThreeWindowPeriod = 500000;
constexpr int64_t kOneWindowPeriod   = 1500000;
constexpr uint32_t kDetect = timedgrid::kDetectUs;   // 1280
}

// ---------------------------------------------------------------------------
// The predicate agrees with the guard band
// ---------------------------------------------------------------------------

TEST(AirChannel, CatchPredicateMatchesTheGuardBand) {
    // TimedGrid derives a symmetric guard G = (W - T_detect)/2 and asserts a
    // frame is caught while |phase error| <= G. That is an ALGEBRAIC claim; the
    // channel decides physically. They must agree, or one of them is fiction.
    const RxWindow w{0, timedgrid::kWindowUs};
    const int64_t expected_t0 = w.open_us + timedgrid::kArmLeadUs;

    for (int32_t e : {-(int32_t) timedgrid::kGuardUs, 0, (int32_t) timedgrid::kGuardUs}) {
        Transmission t; t.t0_us = expected_t0 + e; t.payload_len = 60;
        EXPECT_TRUE(caught(w, t, kDetect)) << "e = " << e;
        EXPECT_TRUE(timedgrid::phaseErrorIsCaught(e)) << "e = " << e;
    }
    for (int32_t e : {-(int32_t) timedgrid::kGuardUs - 1,
                       (int32_t) timedgrid::kGuardUs + 1}) {
        Transmission t; t.t0_us = expected_t0 + e; t.payload_len = 60;
        EXPECT_FALSE(caught(w, t, kDetect)) << "e = " << e;
        EXPECT_FALSE(timedgrid::phaseErrorIsCaught(e)) << "e = " << e;
    }
}

TEST(AirChannel, TheEffectiveCatchWidthIsNarrowerThanTheWindow) {
    // A frame must BOTH start after the window opens AND complete detection
    // before it closes, so the usable width is W - T_detect, not W.
    const RxWindow w{0, timedgrid::kWindowUs};
    int64_t first = -1, last = -1;
    for (int64_t t0 = 0; t0 < 2 * (int64_t) timedgrid::kWindowUs; ++t0) {
        Transmission t; t.t0_us = t0; t.payload_len = 60;
        if (caught(w, t, kDetect)) { if (first < 0) first = t0; last = t0; }
    }
    ASSERT_GE(first, 0);
    EXPECT_EQ(last - first + 1, (int64_t) (timedgrid::kWindowUs - kDetect) + 1);
    EXPECT_EQ(last - first, (int64_t) 2 * timedgrid::kGuardUs);
}

// ---------------------------------------------------------------------------
// Mode A geometry, measured
// ---------------------------------------------------------------------------

TEST(AirChannel, ModeAThreeWindowsMeasured) {
    // Sweep the burst's phase against the window grid in 1 ms steps and count
    // how often at least one of 17 copies is caught.
    uint32_t hit = 0, total = 0;
    for (int64_t phase = 0; phase < kThreeWindowPeriod; phase += 1000) {
        const auto txs = burst(phase, 60);
        const auto win = periodicWindows(0, kThreeWindowPeriod, 4);
        ++total;
        if (run(txs, win, kDetect).windows_hit > 0) ++hit;
    }
    const double p = (double) hit / total;
    // The closed form says 96.4 % using the full window width. Physically the
    // catch width is W - T_detect, so the true figure is ~1 point lower.
    EXPECT_GT(p, 0.93);
    EXPECT_LT(p, 0.97);
}

TEST(AirChannel, ModeAOneWindowIsMuchWorse) {
    uint32_t hit = 0, total = 0;
    for (int64_t phase = 0; phase < kOneWindowPeriod; phase += 1000) {
        const auto txs = burst(phase, 60);
        const auto win = periodicWindows(0, kOneWindowPeriod, 2);
        ++total;
        if (run(txs, win, kDetect).windows_hit > 0) ++hit;
    }
    const double p = (double) hit / total;
    EXPECT_GT(p, 0.28);
    EXPECT_LT(p, 0.36);
    EXPECT_LT(p, 0.5) << "one unsynchronised window is the case Mode B fixes";
}

TEST(AirChannel, ASynchronisedSingleWindowCatchesEverything) {
    // Mode B: one window per round, placed at the node's own T0. Every round
    // must be a hit — that is the entire claim.
    const auto win = periodicWindows(-(int64_t) timedgrid::kArmLeadUs,
                                     timedgrid::kRoundUs, 20);
    std::vector<Transmission> txs;
    for (int r = 0; r < 20; ++r) {
        Transmission t;
        t.t0_us = (int64_t) r * timedgrid::kRoundUs;
        t.payload_len = 60;
        txs.push_back(t);
    }
    const auto res = run(txs, win, kDetect);
    EXPECT_EQ(res.windows_hit, res.windows_armed);
    EXPECT_EQ(res.collided, 0u);
}

TEST(AirChannel, ClockDriftEventuallyWalksOutOfTheWindow) {
    // The whole reason the beacon exists: at +20 ppm the node's window drifts
    // 20 us per second, so after guard/20ppm = 704 s it stops catching.
    const int64_t drift_ppm = 20;
    bool still_hitting_at_half = true, hitting_past_ceiling = false;

    for (int64_t elapsed_s : {350LL, 1400LL}) {
        const int64_t err = elapsed_s * drift_ppm;   // us
        const RxWindow w{0, timedgrid::kWindowUs};
        Transmission t;
        t.t0_us = w.open_us + timedgrid::kArmLeadUs + err;
        t.payload_len = 60;
        if (elapsed_s == 350) still_hitting_at_half = caught(w, t, kDetect);
        else hitting_past_ceiling = caught(w, t, kDetect);
    }
    EXPECT_TRUE(still_hitting_at_half) << "half the ceiling must still work";
    EXPECT_FALSE(hitting_past_ceiling) << "past it, the window is missed";
}

// ---------------------------------------------------------------------------
// Collisions
// ---------------------------------------------------------------------------

TEST(AirChannel, OverlappingTransmittersLoseBothFrames) {
    std::vector<Transmission> txs;
    Transmission a; a.t0_us = 0;      a.payload_len = 60; a.tx_id = 1;
    Transmission b; b.t0_us = 20000;  b.payload_len = 60; b.tx_id = 2;  // inside a's 42 ms
    txs = {a, b};
    const auto win = periodicWindows(-(int64_t) timedgrid::kArmLeadUs, 1000000, 1);
    const auto res = run(txs, win, kDetect);
    EXPECT_EQ(res.collided, 2u);
    EXPECT_EQ(res.windows_hit, 0u) << "a collision loses the frame that would have hit";
}

TEST(AirChannel, OneTransmittersOwnCopiesNeverCollide) {
    // A 17-copy burst is one radio sending sequentially; 88 ms apart against
    // 42 ms of air, they cannot overlap, and must not be modelled as if they do.
    const auto txs = burst(0, 60);
    const auto win = periodicWindows(-(int64_t) timedgrid::kArmLeadUs, 500000, 4);
    EXPECT_EQ(run(txs, win, kDetect).collided, 0u);
}

TEST(AirChannel, ALongFrameCollidesWithMoreOfTheBurst) {
    // 152 B is 95 ms of air — longer than the 88 ms burst stride, so it
    // overlaps two copies rather than fitting between them.
    Transmission big; big.t0_us = 0; big.payload_len = 152; big.tx_id = 2;
    auto txs = burst(0, 60, 17, loratiming::kBurstCopyStrideUs, /*tx_id=*/1);
    txs.push_back(big);
    const auto res = run(txs, {}, kDetect);
    EXPECT_GE(res.collided, 3u) << "the long frame plus both copies it overlaps";
}
