// Mode C, MAC-0: the wake clock.
//
// WakeClockFit.h (hub) pairs the RTC tick count a node reports for its previous
// beacon's T0 with the hub's receive stamp of that beacon, and fits the node's
// wake-clock rate. BeaconTicks.h (node) recovers the tick count at T0 from a
// later (ticks, esp_timer) pair.
//
// Node 2's crystal measured -139.4 ppm: period 16 002 235 Q19. ESP-IDF converts
// deep-sleep requests with the nominal period, so its wakes run at that error.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "BeaconTicks.h"
#include "WakeClockFit.h"

namespace {
constexpr uint32_t kNode2Period = 16002235u;

// RTC ticks a crystal of `period_q19` counts in `us` real microseconds.
uint64_t ticksIn(int64_t us, uint32_t period_q19) {
    return ((uint64_t) us << 19) / period_q19;
}

// Deterministic +-5 ms poll quantisation, the hub's receive-stamp uncertainty.
int64_t pollJitterUs(uint32_t i) {
    uint32_t x = i * 2654435761u + 12345u;
    return (int64_t) (x % 10001u) - 5000;
}

// A run of check-in beacons: one every `interval_us` of real (hub) time,
// optionally dropping one index. Beacon k reports beacon k-1's ticks.
wakeclock::Fit runBeacons(uint32_t count, int64_t interval_us, uint32_t period_q19,
                          bool jitter, int drop_index = -1) {
    wakeclock::Fit fit;
    const uint64_t tick_origin = 5000000ull;   // counter was already running
    uint64_t prev_ticks = 0;
    uint32_t prev_msgid = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const int64_t real_us  = (int64_t) k * interval_us;
        const uint64_t ticks   = tick_origin + ticksIn(real_us, period_q19);
        const uint32_t msgid   = 100 + k;
        const int64_t hub_t0   = 1000000000ll + real_us + (jitter ? pollJitterUs(k) : 0);
        if ((int) k != drop_index) {
            fit.addReported(prev_msgid, prev_ticks);
            fit.noteHeard(msgid, hub_t0);
        }
        prev_ticks = ticks;
        prev_msgid = msgid;
    }
    return fit;
}
}  // namespace

// --- WakeClockFit ------------------------------------------------------------

TEST(WakeClockFit, NoSampleUntilABeaconHasBeenHeard) {
    wakeclock::Fit fit;
    EXPECT_FALSE(fit.addReported(7, 123456));
    EXPECT_EQ(fit.n, 0u);
}

TEST(WakeClockFit, PairsOnlyTheTickCountOfTheBeaconItHeard) {
    wakeclock::Fit fit;
    fit.noteHeard(41, 1000);
    EXPECT_FALSE(fit.addReported(40, 99999)) << "ticks of a different beacon must not pair";
    EXPECT_TRUE(fit.addReported(41, 99999));
    EXPECT_FALSE(fit.addReported(41, 0)) << "0 ticks = no previous beacon since power-on";
}

TEST(WakeClockFit, APerfectCrystalReadsZero) {
    const wakeclock::Fit fit = runBeacons(10, 600000000ll, wakeclock::kNominalPeriodQ19, false);
    ASSERT_EQ(fit.n, 9u);
    EXPECT_NEAR(fit.ppm(), 0, 1);
    EXPECT_EQ(fit.spanS(), 4800u);
}

TEST(WakeClockFit, ASlowCrystalReadsNegative) {
    const wakeclock::Fit fit = runBeacons(10, 600000000ll, kNode2Period, false);
    ASSERT_TRUE(fit.ready());
    EXPECT_NEAR(fit.ppm(), -139, 1)
        << "a node whose crystal is slow believes less time passed: negative = slow";
    EXPECT_NEAR(fit.ppm(), wakeclock::crystalErrorPpm(kNode2Period), 1);
}

TEST(WakeClockFit, HubPollJitterAveragesOutOverARun) {
    const wakeclock::Fit fit = runBeacons(20, 600000000ll, kNode2Period, true);
    ASSERT_EQ(fit.n, 19u);
    EXPECT_NEAR(fit.ppm(), -139, 3) << "+-5 ms per stamp over 3 h is well under 3 ppm";
}

TEST(WakeClockFit, ALostBeaconCostsOneSampleAndNeverMispairs) {
    const wakeclock::Fit fit = runBeacons(10, 600000000ll, kNode2Period, false, /*drop=*/4);
    EXPECT_EQ(fit.n, 7u) << "beacon 4 lost: its own sample and beacon 5's pairing are gone";
    EXPECT_NEAR(fit.ppm(), -139, 1);
}

TEST(WakeClockFit, APowerOnRestartsTheFit) {
    wakeclock::Fit fit = runBeacons(6, 600000000ll, kNode2Period, false);
    ASSERT_EQ(fit.n, 5u);
    // Node powered on: its counter restarted near zero.
    fit.noteHeard(500, 9000000000ll);
    fit.addReported(500, 2000);
    EXPECT_EQ(fit.n, 1u) << "ticks went backwards: old samples describe another counter";
}

TEST(WakeClockFit, CrystalErrorFromPeriod) {
    EXPECT_EQ(wakeclock::crystalErrorPpm(wakeclock::kNominalPeriodQ19), 0);
    EXPECT_EQ(wakeclock::crystalErrorPpm(kNode2Period), -139);
    EXPECT_EQ(wakeclock::crystalErrorPpm(0), 0);
}

// --- BeaconTicks ---------------------------------------------------------------

TEST(BeaconTicks, WalksBackOverTheFramesOwnAirTime) {
    // Read exactly at TxDone: only T0 -> TxDone (61 184 us here) lies between.
    const uint64_t t = beaconticks::ticksAtT0(1000000, 5000000, 5000000, 61184,
                                              wakeclock::kNominalPeriodQ19);
    EXPECT_EQ(t, 1000000u - ((61184ull << 19) / 16000000u));
}

TEST(BeaconTicks, WalksBackOverTaskLatencyToo) {
    // Read 3 ms after TxDone: 64 184 us walked back = 64 184 x 32 768 / 1e6
    // = 2103 ticks at nominal.
    const uint64_t t = beaconticks::ticksAtT0(1000000, 5003000, 5000000, 61184,
                                              wakeclock::kNominalPeriodQ19);
    EXPECT_EQ(t, 1000000u - 2103u);
}

TEST(BeaconTicks, AStampFromTheFutureDoesNotWalkForward) {
    const uint64_t t = beaconticks::ticksAtT0(1000000, 4000000, 5000000, 0, kNode2Period);
    EXPECT_EQ(t, 1000000u);
}

TEST(BeaconTicks, NoPeriodMeansNominal) {
    EXPECT_EQ(beaconticks::ticksAtT0(1000000, 5000000, 5000000, 61184, 0),
              beaconticks::ticksAtT0(1000000, 5000000, 5000000, 61184,
                                     wakeclock::kNominalPeriodQ19));
}
