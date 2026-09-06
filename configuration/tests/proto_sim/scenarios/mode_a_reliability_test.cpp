// Mode A reliability arithmetic.
//
// 100 % cannot be ensured and is not ensured today. The 96.4 % of the geometry
// test is the CHANNEL BEING EMPTY; 433 MHz is shared with weather stations,
// garage remotes and alarm sensors. Reliability comes from acknowledgement and
// retry, which already exist. The frame structure's job is to make the first
// attempt cheap — 42 ms instead of 715 ms — not certain.
//
// See docs/test-plan.md section 4.3 and implementation-plan.md section 4.7.

#include <gtest/gtest.h>

#include <cmath>

#include "TimedGrid.h"

using namespace loratiming;

namespace {

// kOpMaxRetries = 4 at kOpRetryIntervalMs = 3000, so five attempts in total.
constexpr int kAttempts = 5;

double afterRetries(double q, int attempts = kAttempts) {
    return 1.0 - std::pow(1.0 - q, attempts);
}

int roundsTo999(double p) {
    return (int) std::ceil(std::log(0.001) / std::log(1.0 - p));
}

}  // namespace

// ---------------------------------------------------------------------------
// The retry ladder
// ---------------------------------------------------------------------------

TEST(ModeAReliability, RetryLadderExact) {
    EXPECT_NEAR(afterRetries(0.90), 0.99999,  1e-8);
    EXPECT_NEAR(afterRetries(0.50), 0.968750, 1e-8);
    EXPECT_NEAR(afterRetries(0.30), 0.831930, 1e-6);
}

TEST(ModeAReliability, BurstFallbackStrictlyImproves) {
    // The design table's "+ burst fallback" column is NOT derivable from the
    // stated inputs — several combining rules reproduce two of its three rows
    // and none reproduces all three. So this asserts the only load-bearing
    // claim: combining an independent fallback is strictly better, and the
    // improvement grows as the timed attempt gets worse.
    const double p_burst = 482000.0 / 500000.0;   // the geometry test's number
    for (double q : {0.90, 0.50, 0.30}) {
        const double combined = 1.0 - (1.0 - q) * (1.0 - p_burst);
        EXPECT_GT(afterRetries(combined), afterRetries(q)) << q;
        EXPECT_GT(combined, q) << q;
    }
    // Worse timed attempt, bigger relative gain from the fallback.
    const double gain_bad  = (1.0 - (1.0 - 0.30) * (1.0 - p_burst)) - 0.30;
    const double gain_good = (1.0 - (1.0 - 0.90) * (1.0 - p_burst)) - 0.90;
    EXPECT_GT(gain_bad, gain_good);
}

TEST(ModeAReliability, TheFallbackIsNotIndependentDuringABurst) {
    // The last decimal places assume the fallback fails independently of the
    // timed attempt. Against INTERFERENCE that holds. Against the hub running a
    // full-round burst for an unsynced node it does not: that is exactly when a
    // timed node's window is walked through AND when the single serialised TX
    // task is blocked for 1850 ms and cannot issue the fallback. Same cause,
    // so the independence assumption is void in precisely that case.
    const uint32_t burst_span = (kBurstCopies - 1) * kBurstCopyStrideUs + timeOnAirUs(60);
    const uint32_t response_window_us = 400000;
    EXPECT_EQ(burst_span, 1450048u);
    EXPECT_GT(burst_span + response_window_us, timedgrid::kRoundUs)
        << "1850 ms of occupancy against a 1500 ms round is why deferral must "
           "be by TWO rounds, not one";
}

// ---------------------------------------------------------------------------
// How long a confined region would take
// ---------------------------------------------------------------------------

TEST(ModeAReliability, RoundsTo999Percent) {
    // At w = 29 ms (the conservative width the geometry test settles on).
    EXPECT_EQ(roundsTo999(482000.0 / 500000.0), 3);   // 17 copies across the round
    EXPECT_EQ(roundsTo999(3 * 29000.0 / 500000.0), 37);   // 3 copies in 300 ms
    EXPECT_EQ(roundsTo999(5 * 29000.0 / 500000.0), 21);   // 5 copies in 400 ms

    // The design document says "2 rounds — 3 s" for the 17-copy case. It is
    // three: 0.036^2 = 1.296e-3, which is still above the 1e-3 target. The
    // conclusion (a confined region is an order of magnitude worse) is
    // untouched; the cell is off by one.
    EXPECT_GT(std::pow(1.0 - 482000.0 / 500000.0, 2), 0.001);
    EXPECT_LT(std::pow(1.0 - 482000.0 / 500000.0, 3), 0.001);
}

TEST(ModeAReliability, ElapsedTimeNotAirtimeIsWhatMatters) {
    // A burst is 1450 ms and sendTask then blocks 400 ms more, so a clustered
    // event costs 1850 ms per node — 59.2 s for 32 nodes. Mode B serves 10 per
    // round, so the same event is 4 rounds: 6.0 s.
    const double per_node_s = (1450048.0 + 400000.0) / 1e6;
    EXPECT_NEAR(per_node_s * 32, 59.2, 0.1);

    const int rounds = (32 + 10 - 1) / 10;
    EXPECT_EQ(rounds, 4);
    EXPECT_NEAR(rounds * 1.5, 6.0, 1e-9);
}

// ---------------------------------------------------------------------------
// The mixed-mode collision budget
// ---------------------------------------------------------------------------

TEST(ModeAReliability, CollisionBudgetIsAQuarterOfACommandPerDay) {
    // Priority, not a reserved region: the hub runs a normal full-round burst
    // for unsynced nodes and defers colliding private-window traffic.
    const double rounds_per_day = 86400.0 / 1.5;
    EXPECT_NEAR(rounds_per_day, 57600.0, 1e-9);

    const double p_one = 3.5 / rounds_per_day;
    EXPECT_NEAR(p_one, 6.076e-5, 1e-8);

    const double p_any = 1.0 - std::pow(1.0 - p_one, 32);
    EXPECT_NEAR(p_any, 1.943e-3, 1e-6);

    const double bursts_per_day = 32 * 4;          // deep-sleep wakes
    const double collisions = bursts_per_day * p_any * (30.0 / 32.0);
    EXPECT_NEAR(collisions, 0.233, 0.005);
    EXPECT_LT(collisions, 1.0) << "less than one affected command per day";
}

TEST(ModeAReliability, AirtimeBudgetFavoursABroadcastBeaconAt32Nodes) {
    // Same mechanism, opposite sign, purely because of N.
    const double cmds_per_day = 32 * 3.5;
    const double beacons_per_day = 86400.0 / 348.0;      // every 5.8 min

    const double today     = cmds_per_day * kBurstCopies * timeOnAirUs(60) / 1e6;
    const double broadcast = cmds_per_day * timeOnAirUs(60) / 1e6
                           + beacons_per_day * timeOnAirUs(45) / 1e6;
    const double unicast   = cmds_per_day * timeOnAirUs(60) / 1e6
                           + beacons_per_day * 32 * timeOnAirUs(45) / 1e6;

    EXPECT_NEAR(today,     80.06,  0.05);
    EXPECT_NEAR(broadcast, 13.12,  0.05);
    EXPECT_NEAR(unicast,   273.69, 0.5);

    // The decision this encodes: a unicast keepalive is not viable at 32 nodes.
    EXPECT_GT(unicast, today);
    EXPECT_LT(broadcast, today);
}
