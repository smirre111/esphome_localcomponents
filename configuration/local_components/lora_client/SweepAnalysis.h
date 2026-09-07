#pragma once

#include <stdint.h>

#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// SweepAnalysis — turning an arm-offset sweep into T_detect (HW-2).
//
// WHY THIS MEASUREMENT EXISTS. Every guard-band figure in the design rests on
// T_detect, and T_detect is currently a LoRaWAN rule of thumb (5 symbols),
// not a datasheet value — and this system never writes RegDetectionOptimize or
// RegDetectThreshold, so they sit at reset defaults nobody has checked. It sets
// the entire late-side margin. A field failure would surface late, across 32
// nodes, and be unattributable.
//
// HOW IT WORKS. Arm the receive window deliberately early or late by `l`
// against a transmission whose T0 is known, and ask whether the frame is
// caught. With the window open at T0 - (T_pre + G) + l:
//
//     caught  <=>  -G <= l <= +G
//
// Both edges fall at +/-G, so the sweep measures the WIDTH between them:
//
//     span = 2G = W - T_detect      =>      T_detect = W - span
//
// W is known exactly (symTimeout * T_sym), so the sweep yields T_detect
// directly. Nothing here assumes a value for it — which is the point: an
// analysis that used the assumed 5 symbols to find the edges would be circular.
//
// The two edges are also a CONSISTENCY CHECK on each other. They must be
// symmetric about zero to within the step size; if they are not, the window is
// not where the node thinks it is, and the number to fix is the arm lead rather
// than T_detect.
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace sweep
{

// One step of the sweep: how many marks were armed at this offset and how many
// were caught.
struct Point
{
    int32_t  offset_us{0};
    uint32_t armed{0};
    uint32_t hit{0};

    // A step counts as "reception works here" only on a clear majority. Right
    // at an edge the answer is genuinely marginal, and a single frame either
    // way must not move the result.
    bool receives(uint32_t min_armed = 8, uint32_t pct = 80) const
    {
        return armed >= min_armed && (hit * 100u) >= (armed * pct);
    }
};

struct Result
{
    bool     valid{false};
    int32_t  early_edge_us{0};   // most-negative offset that still receives
    int32_t  late_edge_us{0};    // most-positive offset that still receives
    int32_t  span_us{0};         // late - early  == 2G
    int32_t  detect_us{0};       // W - span
    int32_t  asymmetry_us{0};    // early + late; should be ~0
    uint32_t receiving_points{0};
};

// `points` must be sorted by offset. `window_us` is the width actually armed
// (symTimeout * T_sym), which the caller knows exactly.
inline Result analyse(const Point *points, uint32_t n, uint32_t window_us)
{
    Result r;
    if (points == nullptr || n == 0)
        return r;

    bool found = false;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!points[i].receives())
            continue;
        r.receiving_points++;
        if (!found)
        {
            r.early_edge_us = points[i].offset_us;
            found = true;
        }
        r.late_edge_us = points[i].offset_us;
    }
    if (!found)
        return r;   // nothing received anywhere: not a measurement, a failure

    r.span_us      = r.late_edge_us - r.early_edge_us;
    r.asymmetry_us = r.early_edge_us + r.late_edge_us;

    // T_detect = W - span. A span WIDER than the window is impossible and means
    // the sweep is measuring something else (a second frame arriving, a window
    // that never actually closed), so refuse rather than report a negative
    // detection time as if it were a result.
    const int32_t detect = (int32_t) window_us - r.span_us;
    if (detect < 0)
        return r;

    r.detect_us = detect;
    r.valid     = true;
    return r;
}

// The rule of thumb the design currently assumes, for comparison only. Nothing
// in analyse() uses it.
static constexpr int32_t kAssumedDetectUs = (int32_t) timedgrid::kDetectUs;

// How far the measurement may fall from the assumption before the design's
// guard band needs revisiting. G = (W - T_detect)/2, so an error of d in
// T_detect moves each edge by d/2.
inline bool agreesWithAssumption(const Result &r, int32_t tolerance_us = 640)
{
    if (!r.valid) return false;
    const int32_t d = r.detect_us - kAssumedDetectUs;
    return d <= tolerance_us && d >= -tolerance_us;
}

}  // namespace sweep
