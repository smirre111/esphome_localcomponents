// SleepClockCorrection.h: the node's correction for what ESP-IDF 6.0 adds to
// esp_timer on each light sleep (nominal 32 kHz period, truncated per sleep)
// against what the measured crystal really took.
//
// The numbers are node 2's: +60 ppm under auto light sleep, +9 ppm with sleep
// off. A crystal 55 ppm fast measures 16 000 000 / 1.000055 = 15 999 120 Q19.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "SleepClockCorrection.h"

using namespace sleepclock;

namespace {
constexpr uint32_t kFast55 = 15999120u;   // 55 ppm fast
constexpr uint32_t kSlow55 = 16000880u;   // 55 ppm slow

// The true duration of `ticks`, in Q19 us, for a crystal of the given period.
int64_t trueQ19(uint64_t ticks, uint32_t period) { return (int64_t) (ticks * period); }
}  // namespace

TEST(SleepClockCorrection, IdfAddsTheNominalPeriodTruncated) {
    EXPECT_EQ(idfAddedUs(0), 0);
    EXPECT_EQ(idfAddedUs(1), 30) << "30.517578125 us truncated";
    EXPECT_EQ(idfAddedUs(32768), 1000000) << "one nominal second is exact";
}

TEST(SleepClockCorrection, CrystalErrorFromTheMeasuredPeriod) {
    EXPECT_EQ(crystalErrorPpb(kNominalPeriodQ19), 0);
    EXPECT_NEAR(crystalErrorPpb(kFast55), 55003, 1);
    EXPECT_NEAR(crystalErrorPpb(kSlow55), -54997, 1);
    EXPECT_EQ(crystalErrorPpb(0), 0) << "no calibration read: no claim";
}

TEST(SleepClockCorrection, AFastCrystalMakesEspTimerRunAheadByItsPpmOfSleep) {
    Accumulator a;
    a.noteSleep(32768, kFast55);           // one nominal second asleep
    EXPECT_EQ(a.overcount_us, 55) << "880 x 32768 / 2^19 = 55.0 us ahead";
    EXPECT_EQ(a.nodeTimeUs(2000000), 2000000 - 55) << "node time takes the lead back out";
}

TEST(SleepClockCorrection, ASlowCrystalMakesEspTimerFallBehind) {
    Accumulator a;
    a.noteSleep(32768, kSlow55);
    EXPECT_EQ(a.overcount_us, -55);
    EXPECT_EQ(a.nodeTimeUs(2000000), 2000000 + 55);
}

TEST(SleepClockCorrection, APerfectCrystalStillLosesTheTruncation) {
    // 10 000 one-tick sleeps: IDF adds 30 us each, the crystal took 30.5176.
    Accumulator a;
    for (int i = 0; i < 10000; ++i) a.noteSleep(1, kNominalPeriodQ19);
    // lost = 10 000 x 0.517578125 = 5175.78 us -> overcount floor(-5175.78)
    EXPECT_EQ(a.overcount_us, -5176)
        << "esp_timer falls behind by the truncated fractions; node time adds them back";
}

TEST(SleepClockCorrection, SubMicrosecondErrorsAccumulateInsteadOfVanishing) {
    // 100 000 sleeps of 10 ticks: each sleep's crystal error is 0.017 us, far
    // under one. Truncating per sleep would report zero forever.
    Accumulator a;
    for (int i = 0; i < 100000; ++i) a.noteSleep(10, kFast55);
    const int64_t expect_q19 =
        100000 * ((idfAddedUs(10) << kCalFractBits) - trueQ19(10, kFast55));
    EXPECT_EQ(a.overcount_us, expect_q19 >> kCalFractBits);
    EXPECT_NE(a.overcount_us, 0);
}

TEST(SleepClockCorrection, NoCalibrationCountsTheSleepButCorrectsNothing) {
    Accumulator a;
    a.noteSleep(32768, 0);
    EXPECT_EQ(a.overcount_us, 0);
    EXPECT_EQ(a.sleeps, 1u);
    EXPECT_EQ(a.slept_ticks, 32768u);
}

// The run that motivated it: 900 s, ~93% asleep in 30 ms sleeps, crystal +55
// ppm. esp_timer is advanced exactly as IDF does it; node time must land on the
// crystal's own elapsed time to within a microsecond, and the whole ~+46 ms
// esp_timer lead must be gone.
TEST(SleepClockCorrection, ANineHundredSecondProductionRunLandsOnTheCrystal) {
    Accumulator a;
    int64_t esp_timer_q19 = 0;   // what IDF advanced esp_timer by, in Q19
    int64_t truth_q19     = 0;
    const uint64_t ticks_per_sleep = 983;        // ~30 ms
    const int sleeps = 25540;                    // ~837 s asleep of 900
    for (int i = 0; i < sleeps; ++i) {
        esp_timer_q19 += idfAddedUs(ticks_per_sleep) << kCalFractBits;
        truth_q19     += trueQ19(ticks_per_sleep, kFast55);
        a.noteSleep(ticks_per_sleep, kFast55);
    }
    const int64_t esp_us   = esp_timer_q19 >> kCalFractBits;
    const int64_t truth_us = truth_q19 >> kCalFractBits;
    EXPECT_GT(esp_us - truth_us, 20000) << "precondition: uncorrected, esp_timer is tens of ms ahead";
    EXPECT_LE(std::llabs(a.nodeTimeUs(esp_us) - truth_us), 1);
}
