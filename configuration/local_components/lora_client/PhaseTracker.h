#pragma once

#include <stdint.h>

#include "LoraTiming.h"

// ---------------------------------------------------------------------------
// PhaseTracker — where the node thinks the hub's frames land, and how sure it
// is. Feeds the beacon fields the hub uses to decide whether a node may be
// promoted (implementation-plan.md 4.6, B2).
//
// THE SAMPLE MUST BE FILTERED BY ADDRESS BEFORE IT IS TAKEN, and this is the
// whole reason the class exists rather than a pair of members on the
// dispatcher.
//
// noteDriftSample() is called BEFORE parsing, by design: a drift measurement
// only cares when a frame ARRIVED, and every step after that point can
// legitimately discard it (CRC, replay, a plaintext frame rejected because the
// session requires encryption). Taking the timestamp early is correct for
// drift. It is WRONG for phase: a node that stamps its neighbour's frames is
// measuring a transmission aimed at a slot 46.875 ms away from its own, so
// phaseErrUs comes out BIMODAL — a cluster at 0 and a cluster at +/- one slot
// pitch — and the mean of those two clusters is a number that describes
// nothing. On a shared broadcast channel with 32 nodes, most frames a node
// hears are somebody else's.
//
// So: the arrival instant is captured early (correct), and COMMITTED here only
// once the frame is known to be addressed to this node (also correct). The two
// are different decisions and the code now keeps them apart.
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace phase
{

// Where the node's RTC slow clock actually comes from. Mode B is gated on the
// external crystal: on the internal RC (~5 %) a node cannot hold phase between
// beacons, and must stay in Mode A VISIBLY rather than silently failing to
// hear anything.
enum class RtcSlowSrc : uint8_t { Unknown = 0, InternalRc = 1, Crystal = 2, Ext8MD256 = 3 };

// Map the SoC's own RTC-slow mux value onto the enum above.
//
// THE TWO ENUMERATIONS DO NOT AGREE, and a cast is therefore a live bug rather
// than a shortcut. ESP-IDF's soc_rtc_slow_clk_src_t (soc/clk_tree_defs.h) is
// numbered to match the register field:
//
//     RC_SLOW = 0    XTAL32K = 1    RC_FAST_D256 = 2
//
// while RtcSlowSrc above is numbered for the wire. So a cast turns XTAL32K
// into InternalRc, and — the dangerous direction — RC_FAST_D256 into Crystal.
// That second one reports rtcSlowSrc = 2 to the hub, passes every gate that
// asks for the crystal, and runs Mode B on a divided internal oscillator: the
// exact failure the gate exists to prevent, wearing the gate's own pass value.
//
// Takes a plain integer rather than the IDF type so this header stays
// dependency-free and the mapping is testable on the host, where neither the
// IDF enum nor the register exists.
constexpr RtcSlowSrc rtcSlowSrcFromSocValue(uint8_t soc_value)
{
    switch (soc_value)
    {
        case 0:  return RtcSlowSrc::InternalRc;   // SOC_RTC_SLOW_CLK_SRC_RC_SLOW
        case 1:  return RtcSlowSrc::Crystal;      // SOC_RTC_SLOW_CLK_SRC_XTAL32K
        case 2:  return RtcSlowSrc::Ext8MD256;    // SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256
        default: return RtcSlowSrc::Unknown;      // incl. SOC_RTC_SLOW_CLK_SRC_INVALID
    }
}

struct Sample
{
    int64_t t0_measured_us{0};   // recovered SFD end, node clock
    int64_t t0_expected_us{0};   // where the grid said it would be
};

// Running summary. Distribution, not a mean: the design's gate is a p99 and a
// SHAPE (unimodal), and a mean would hide exactly the bimodality above.
struct Stats
{
    uint32_t n{0};
    int32_t  last_us{0};
    int32_t  min_us{INT32_MAX};
    int32_t  max_us{INT32_MIN};
    int64_t  sum_us{0};
    // Samples outside the guard band. A promotion criterion that ignored these
    // would promote a node whose window is already missing frames.
    uint32_t outside_guard{0};

    void reset() { *this = Stats{}; }

    bool valid() const { return n > 0; }
    int32_t mean_us() const { return n == 0 ? 0 : (int32_t) (sum_us / (int64_t) n); }
    // Peak-to-peak. Cheap, and it is what exposes a bimodal distribution: two
    // clusters one slot pitch apart show up here as ~46875 even when the mean
    // sits innocently near zero.
    int32_t spread_us() const { return n == 0 ? 0 : (max_us - min_us); }
};

// Commit one sample. `guard_us` is the half-width the window tolerates.
inline void commit(Stats &s, const Sample &sample, uint32_t guard_us)
{
    const int64_t err64 = sample.t0_measured_us - sample.t0_expected_us;
    // Clamp before narrowing: a nonsense pair (an uninitialised expectation, a
    // frame from before the anchor) must not wrap into a plausible-looking
    // small error.
    const int32_t err = err64 > INT32_MAX   ? INT32_MAX
                      : err64 < INT32_MIN   ? INT32_MIN
                                            : (int32_t) err64;
    s.n++;
    s.last_us = err;
    if (err < s.min_us) s.min_us = err;
    if (err > s.max_us) s.max_us = err;
    s.sum_us += err;
    if (err > (int32_t) guard_us || err < -(int32_t) guard_us)
        s.outside_guard++;
}

// Recover T0 from a completed reception. One line, by construction — see
// LoraTiming.h on why this must never be decomposed.
inline int64_t t0FromRx(int64_t t_rxdone_us, uint32_t payload_len)
{
    return loratiming::t0FromRxDoneUs(t_rxdone_us, payload_len);
}

// Is this node's phase good enough to be trusted for a timed window?
//
// Requires a real sample count, every sample inside the guard, AND a spread
// that is small compared with the guard. The spread test is what rejects a
// bimodal distribution whose mean happens to look fine.
inline bool phaseTrustworthy(const Stats &s, uint32_t guard_us,
                             uint32_t min_samples = 8)
{
    if (s.n < min_samples)          return false;
    if (s.outside_guard != 0)       return false;
    return (uint32_t) s.spread_us() <= guard_us;
}

}  // namespace phase
