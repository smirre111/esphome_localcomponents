// SleepClockCorrection.h: the node's MAC-0 timebase across light sleep.
// Sleep time comes from the RTC tick counter (32 kHz crystal x measured
// period); after each sleep node time is re-anchored to it absolutely.
//
// Node 2's crystal measured -139.4 ppm: period 16 002 235 Q19.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "SleepClockCorrection.h"

using namespace sleepclock;

namespace {
constexpr uint32_t kNode2 = 16002235u;   // -139.4 ppm, slow
}  // namespace

TEST(SleepClockCorrection, CrystalErrorFromTheMeasuredPeriod) {
    EXPECT_EQ(crystalErrorPpb(kNominalPeriodQ19), 0);
    EXPECT_NEAR(crystalErrorPpb(kNode2), -139667, 1);
    EXPECT_NEAR(crystalErrorPpb(15999120u), 55003, 1);
    EXPECT_EQ(crystalErrorPpb(0), 0);
}

TEST(SleepClockCorrection, TicksBecomeCrystalTimeWithTheMeasuredPeriod) {
    RtcTimebase tb;
    tb.start(/*ticks=*/1000, /*us=*/5000000, kNode2);
    EXPECT_EQ(tb.usAt(1000), 5000000);
    // One nominal second of ticks on a crystal 139.7 ppm slow took longer.
    EXPECT_EQ(tb.usAt(1000 + 32768), 5000000 + 1000139);
}

TEST(SleepClockCorrection, ConvertsFromTheBaseSoLongUptimesDoNotOverflow) {
    RtcTimebase tb;
    const uint64_t ticks_at_100_days = 32768ull * 86400ull * 100ull;
    tb.start(ticks_at_100_days, 0, kNode2);
    const uint64_t day = 32768ull * 86400ull;
    // 86 400 s x (1 + 139.7e-6) = 86 412.07 s
    EXPECT_NEAR((double) tb.usAt(ticks_at_100_days + day), 86412067000.0, 2000.0);
}

TEST(SleepClockCorrection, ARecalibrationDoesNotMoveTheClock) {
    RtcTimebase tb;
    tb.start(0, 0, 16002314u);                       // boot calibration
    const uint64_t t60 = 32768ull * 60ull;
    const int64_t before = tb.usAt(t60);
    tb.rebase(t60, kNode2);                          // 60 s recalibration
    EXPECT_EQ(tb.usAt(t60), before) << "no jump at the instant the period changes";
    EXPECT_EQ(tb.usAt(t60 + 32768) - before, 1000139) << "the new period applies afterwards";
}

TEST(SleepClockCorrection, OffsetAndEspTimerErrorAreMeasuredFromThePair) {
    EXPECT_EQ(offsetFor(/*rtc_us=*/10000500, /*esp=*/10000000), 500);
    // esp_timer gained 190 ms on the crystal over 1000 s: offset fell by 190 000 us.
    EXPECT_EQ(espTimerErrorPpm(0, -190000, 1000000000), 190);
    EXPECT_EQ(espTimerErrorPpm(0, 139000, 1000000000), -139);
    EXPECT_EQ(espTimerErrorPpm(0, -1, 0), 0) << "no span, no claim";
}

// The failure that motivated it: IDF's per-sleep bookkeeping is off by some
// amount the node cannot see (here +42 us a sleep, the size the +202 ppm run
// implies). Re-anchoring after each sleep must land node time on the crystal
// after 4000 sleeps - to within one tick's quantisation, not 4000 x 42 us.
TEST(SleepClockCorrection, ReanchoringAfterEverySleepCannotAccumulateIdfError) {
    RtcTimebase tb;
    tb.start(0, 0, kNode2);

    uint64_t ticks = 0;           // the RTC counter: runs through everything
    int64_t  esp   = 0;           // esp_timer, as IDF maintains it
    int64_t  offset = 0;
    constexpr int64_t kIdfErrorPerSleepUs = 42;
    for (int i = 0; i < 4000; ++i) {
        // awake 7 ms: both clocks run (esp_timer on the 40 MHz crystal)
        const uint64_t awake_ticks = 229;
        esp   += (int64_t) ((awake_ticks * kNode2) >> kCalFractBits);
        ticks += awake_ticks;
        // asleep 215 ms: esp_timer stopped, then set by IDF - with its error
        const uint64_t sleep_ticks = 7045;
        esp   += (int64_t) ((sleep_ticks * kNominalPeriodQ19) >> kCalFractBits) + kIdfErrorPerSleepUs;
        ticks += sleep_ticks;
        // exit hook: one pair, absolute re-anchor
        offset = offsetFor(tb.usAt(ticks), esp);
    }
    const int64_t node_us  = esp + offset;
    const int64_t crystal  = tb.usAt(ticks);
    // +42 us x 4000 sleeps against -140 ppm of nominal-period loss: ~44 ms.
    EXPECT_GT(std::llabs(esp - crystal), 20000) << "precondition: esp_timer alone is far off";
    EXPECT_LE(std::llabs(node_us - crystal), 31) << "node time is crystal time to one tick";
}

// --- Deep sleep request ------------------------------------------------------

TEST(SleepClockCorrection, NoMeasuredPeriodRequestsAsAsked) {
    EXPECT_EQ(deepSleepRequestUs(900'000'000ull, 0), 900'000'000ull);
    EXPECT_EQ(deepSleepRequestUs(900'000'000ull, kNominalPeriodQ19), 900'000'000ull);
}

TEST(SleepClockCorrection, ASlowCrystalIsAskedForLessSoTheRealSleepIsRight) {
    // Node 2: 15 min wanted. IDF turns the request into ticks at the nominal
    // period, and the crystal takes longer per tick.
    constexpr uint64_t want = 900'000'000ull;
    const uint64_t req = deepSleepRequestUs(want, kNode2);
    EXPECT_LT(req, want);
    // What the node really sleeps: ticks at nominal, times the real period.
    const uint64_t ticks = (req << kCalFractBits) / kNominalPeriodQ19;
    const uint64_t real  = (ticks * kNode2) >> kCalFractBits;
    EXPECT_LE((int64_t) (real > want ? real - want : want - real), 100)
        << "within 100 us of 15 min, where the uncorrected request is 125 ms late";
    const uint64_t ticks_raw = (want << kCalFractBits) / kNominalPeriodQ19;
    const uint64_t real_raw  = (ticks_raw * kNode2) >> kCalFractBits;
    EXPECT_GT(real_raw - want, 120'000u) << "precondition: uncorrected is ~125 ms late";
}

TEST(SleepClockCorrection, ASixHourSleepDoesNotOverflow) {
    constexpr uint64_t six_h = 6ull * 3600ull * 1'000'000ull;
    const uint64_t req = deepSleepRequestUs(six_h, kNode2);
    EXPECT_NEAR((double) req, (double) six_h * 16000000.0 / 16002235.0, 2.0);
}
