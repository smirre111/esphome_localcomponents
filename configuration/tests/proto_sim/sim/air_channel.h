#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>

#include "LoraTiming.h"
#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// AirChannel — a radio that can MISS a frame.
//
// SimRadio is a perfect broadcast medium: every sink receives every frame, with
// no notion of time. That is right for protocol tests and useless for timing
// ones — reception geometry, window miss rate and burst behaviour are all
// invisible to it, and two of the defects found by review lived exactly there.
//
// This models the part SimRadio deliberately omits:
//
//   * a transmission OCCUPIES AIR for a real duration, positioned by its T0
//   * a receiver is open only inside a window
//   * a frame is caught only if it both STARTS after the window opens and
//     completes DETECTION before the window closes
//   * overlapping transmissions collide and neither is caught
//
// The catch predicate is deliberately physical rather than a restatement of the
// guard band, so that the guard band can be CHECKED against it rather than
// assumed. A test asserts the two agree at |e| <= G.
//
// Deliberately NOT modelled: capture effect (a strong frame winning a
// collision), partial-preamble detection, RSSI/SNR thresholds. Every one of
// those makes reception BETTER than this model says, so results here are a
// lower bound — which is the safe direction for a design decision.
// ---------------------------------------------------------------------------

namespace proto_sim {

struct Transmission {
    int64_t  t0_us{0};        // SFD end — the reference point, as everywhere
    uint32_t payload_len{60};
    int      tx_id{0};        // who transmitted; frames from one sender never
                              // collide with each other in this model
    uint32_t seq{0};

    int64_t air_start_us() const {
        return t0_us - (int64_t) loratiming::kPreambleToT0Us;
    }
    int64_t air_end_us() const {
        return t0_us + (int64_t) loratiming::t0ToRxDoneUs(payload_len);
    }
};

struct RxWindow {
    int64_t open_us{0};
    int64_t width_us{timedgrid::kWindowUs};

    int64_t close_us() const { return open_us + width_us; }
};

// The catch predicate, stated physically.
//
//   - the preamble must not have started before the receiver opened, or the
//     receiver missed the beginning of it and cannot lock
//   - detection must complete before the receiver closes
inline bool caught(const RxWindow& w, const Transmission& t, uint32_t detect_us) {
    const int64_t air_start = t.air_start_us();
    return air_start >= w.open_us &&
           (air_start + (int64_t) detect_us) <= w.close_us();
}

// Two transmissions collide if their air time overlaps at all. Same-sender
// frames never collide: one radio transmits one frame at a time.
inline bool collides(const Transmission& a, const Transmission& b) {
    if (a.tx_id == b.tx_id) return false;
    return a.air_start_us() < b.air_end_us() && b.air_start_us() < a.air_end_us();
}

struct ChannelResult {
    uint32_t offered{0};        // transmissions placed on the air
    uint32_t windows_armed{0};
    uint32_t windows_hit{0};    // windows that caught at least one frame
    uint32_t collided{0};       // transmissions lost to a collision
};

// Run one experiment: a set of transmissions against a set of windows.
inline ChannelResult run(const std::vector<Transmission>& txs,
                         const std::vector<RxWindow>& windows,
                         uint32_t detect_us = timedgrid::kDetectUs) {
    ChannelResult r;
    r.offered = (uint32_t) txs.size();
    r.windows_armed = (uint32_t) windows.size();

    std::vector<bool> lost(txs.size(), false);
    for (size_t i = 0; i < txs.size(); ++i)
        for (size_t j = i + 1; j < txs.size(); ++j)
            if (collides(txs[i], txs[j])) { lost[i] = true; lost[j] = true; }
    r.collided = (uint32_t) std::count(lost.begin(), lost.end(), true);

    for (const auto& w : windows) {
        for (size_t i = 0; i < txs.size(); ++i) {
            if (lost[i]) continue;
            if (caught(w, txs[i], detect_us)) { r.windows_hit++; break; }
        }
    }
    return r;
}

// A hub burst: `copies` transmissions `stride_us` apart, starting at `first_t0`.
inline std::vector<Transmission> burst(int64_t first_t0_us, uint32_t payload_len,
                                       int copies = loratiming::kBurstCopies,
                                       uint32_t stride_us = loratiming::kBurstCopyStrideUs,
                                       int tx_id = 0) {
    std::vector<Transmission> v;
    for (int i = 0; i < copies; ++i) {
        Transmission t;
        t.t0_us = first_t0_us + (int64_t) i * stride_us;
        t.payload_len = payload_len;
        t.tx_id = tx_id;
        t.seq = (uint32_t) i;
        v.push_back(t);
    }
    return v;
}

// A node's Mode A listening pattern: `count` windows, `period_us` apart.
inline std::vector<RxWindow> periodicWindows(int64_t first_open_us,
                                             int64_t period_us, int count,
                                             int64_t width_us = timedgrid::kWindowUs) {
    std::vector<RxWindow> v;
    for (int i = 0; i < count; ++i)
        v.push_back(RxWindow{first_open_us + (int64_t) i * period_us, width_us});
    return v;
}

}  // namespace proto_sim
