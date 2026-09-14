#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// WakeClockFit - Mode C, MAC-0: the rate of a node's wake clock against the hub.
//
// A Class A node wakes on its RTC timer, which counts the 32 kHz crystal. ESP-IDF
// converts a requested sleep to ticks with the NOMINAL period, so the node's
// idea of "sleep 600 s" runs at the crystal's error. Each wake beacon reports
// the RTC tick count at the PREVIOUS beacon's T0, with that beacon's msgId; the
// hub pairs it with its own receive stamp of the same beacon.
//
//   x = hub microseconds since the first paired beacon
//   y = node ticks since then, times the NOMINAL period (what the node believes)
//   ppm = (slope - 1) x 1e6   positive = node clock FAST (DriftEstimator's sign)
//
// The hub stamp is quantised by its poll loop (about +-5 ms); over a 10 min
// check-in that is +-8 ppm per pair, and the least-squares slope over the run
// is what gets reported.
//
// Pairing is by msgId, so a lost beacon costs one sample and never pairs a
// tick count with the wrong frame. A tick count lower than the last one means
// the node powered on (the counter reset): the fit starts over.
// ---------------------------------------------------------------------------

namespace wakeclock
{

constexpr uint32_t kNominalPeriodQ19 = 16000000u;
constexpr int      kCalFractBits     = 19;

struct Fit
{
    // The beacon most recently heard, waiting for its tick count.
    bool     have_heard{false};
    uint32_t heard_msgid{0};
    int64_t  heard_hub_t0_us{0};

    // Origin of the fit and least-squares sums, in microseconds from it.
    bool     have_origin{false};
    uint64_t origin_ticks{0};
    int64_t  origin_hub_us{0};
    uint64_t last_ticks{0};
    int64_t  last_x_us{0};
    double   sx{0}, sy{0}, sxx{0}, sxy{0};
    uint32_t n{0};

    void reset()
    {
        have_origin = false;
        sx = sy = sxx = sxy = 0;
        n = 0;
        last_ticks = 0;
        last_x_us = 0;
    }

    // This beacon reported the tick count of the PREVIOUS one. Returns true
    // when it became a sample. Call before noteHeard() for the same beacon.
    bool addReported(uint32_t prev_msgid, uint64_t prev_t0_ticks)
    {
        if (!have_heard || prev_t0_ticks == 0 || prev_msgid != heard_msgid)
            return false;

        if (have_origin && prev_t0_ticks <= last_ticks)
            reset();   // the RTC counter went backwards: the node powered on

        if (!have_origin)
        {
            have_origin   = true;
            origin_ticks  = prev_t0_ticks;
            origin_hub_us = heard_hub_t0_us;
        }

        const double x = (double) (heard_hub_t0_us - origin_hub_us);
        const double y = (double) (((prev_t0_ticks - origin_ticks) * (uint64_t) kNominalPeriodQ19)
                                   >> kCalFractBits);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
        n++;
        last_ticks = prev_t0_ticks;
        last_x_us  = heard_hub_t0_us - origin_hub_us;
        return true;
    }

    // This beacon was heard at hub T0 with this msgId.
    void noteHeard(uint32_t msgid, int64_t hub_t0_us)
    {
        have_heard      = true;
        heard_msgid     = msgid;
        heard_hub_t0_us = hub_t0_us;
    }

    bool ready() const { return n >= 2; }

    int32_t ppm() const
    {
        if (n < 2) return 0;
        const double nn    = (double) n;
        const double denom = nn * sxx - sx * sx;
        if (denom <= 0.0) return 0;
        return (int32_t) (((nn * sxy - sx * sy) / denom - 1.0) * 1e6);
    }

    uint32_t spanS() const { return (uint32_t) (last_x_us / 1000000); }
};

// Mode C's pass line: how late or early the node woke against what it WANTED,
// in ppm of the sleep, positive = node early (its clock fast, the ModeTest sign).
//
//   applied_us  what the node handed ESP-IDF; IDF turns it into ticks at the
//               nominal period, so it is also the sleep in NOMINAL node us
//   counter_ppm the fit above: node nominal us against hub us, positive = fast
//   real sleep in hub us = applied_us / (1 + counter_ppm)
//   error_ppm            = requested_us / real - 1
//                        = (1 + counter_ppm) * requested_us / applied_us - 1
//
// Uncorrected (applied == requested) this IS the counter rate. With the node's
// correction it is what is left: the calibration's reference (the 40 MHz
// crystal) against the hub.
inline int32_t wakeTimingErrorPpm(uint64_t requested_us, uint64_t applied_us,
                                  int32_t counter_ppm)
{
    if (requested_us == 0 || applied_us == 0) return 0;
    const double e = (1.0 + counter_ppm * 1e-6) * (double) requested_us / (double) applied_us - 1.0;
    return (int32_t) (e * 1e6 + (e >= 0 ? 0.5 : -0.5));
}

// The crystal error a measured period implies, positive = fast: what ppm()
// should come out as when nothing but the nominal-period conversion is wrong.
constexpr int32_t crystalErrorPpm(uint32_t period_q19)
{
    if (period_q19 == 0) return 0;
    return (int32_t) ((((int64_t) kNominalPeriodQ19 - (int64_t) period_q19) * 1000000LL)
                      / (int64_t) period_q19);
}

}  // namespace wakeclock
