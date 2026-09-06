#pragma once

#include <stdint.h>

#include "LoraTiming.h"

// ---------------------------------------------------------------------------
// TimedGrid — the Mode B slot geometry.
//
//   T0_k(n) = A + n * kRoundUs + k * kSlotPitchUs        k = 0 .. kSlotCount-1
//
// A is the grid anchor: set once when the grid starts and NEVER moved. A hub
// restart therefore invalidates every node's phase, which is why the design
// requires a broadcast demote on startup rather than silently re-anchoring.
//
//  round n, 1500 ms
//  |<------------------------------------------------------------->|
//  |  k=0     k=1     k=2     k=3                            k=31   |
//  |  [29ms]  [29ms]  [29ms]  [29ms]        ...              [29ms]  |
//  |  <-46.875 ms->        17.435 ms clear between adjacent windows  |
//
// Dependency-free, like LoraTiming.h. See implementation-plan.md sections
// 4.2-4.4 and 4.7, and test-plan.md section 5.2.
// ---------------------------------------------------------------------------

namespace timedgrid
{

using namespace loratiming;

static constexpr uint32_t kRoundUs   = 1500000;
static constexpr uint32_t kSlotCount = 32;

// The pitch must divide the round exactly. It does at 32 slots (46 875 us); at
// e.g. depth 7 or 48 it would not, and the grid would walk.
static constexpr uint32_t kSlotPitchUs = kRoundUs / kSlotCount;
static_assert(kRoundUs % kSlotCount == 0, "slot pitch must divide the round exactly");

// --- The receive window ---------------------------------------------------
//
// The window width is the radio's symbol timeout, which today is
// symTimeout = int(30.0f / 0.26f) = 115 in LoraInterface.cpp. That expression
// came from a comment claiming "20 ms per packet"; real frames are 42-95 ms, so
// the 29.44 ms window is an accident that happens to work. It is named here so
// that changing it is a deliberate act with a visible diff.
static constexpr uint32_t kSymbolTimeoutSymbols = 115;
static constexpr uint32_t kWindowUs = kSymbolTimeoutSymbols * kSymbolUs;

// Time from the first preamble chirp reaching the radio to detection being
// latched, i.e. how much of the window a frame must overlap to be caught.
//
// FIVE SYMBOLS IS A LoRaWAN RULE OF THUMB, NOT A DATASHEET NUMBER, and this
// system never writes RegDetectionOptimize / RegDetectThreshold, so they sit at
// reset defaults. It sets the ENTIRE late-side margin below. Measured by
// test-plan.md section 10.6 HW-2 (sweep the arm offset late until reception
// fails; the failure edge is this number). Until then, every guard figure that
// depends on it is an assumption wearing a number's clothes.
static constexpr uint32_t kDetectSymbolsAssumed = 5;
static constexpr uint32_t kDetectUs = kDetectSymbolsAssumed * kSymbolUs;

// The guard band falls out of the window and the detection time, symmetrically:
//
//   early by e: the preamble starts at T0 - T_pre - e, inside while e <= G
//   late by  l: detection completes at T0 - T_pre + l + T_detect, before close
//               while l <= G
static constexpr uint32_t kGuardUs = (kWindowUs - kDetectUs) / 2;

// Arm the receiver this far before T0.
static constexpr uint32_t kArmLeadUs = kPreambleToT0Us + kGuardUs;

constexpr int64_t windowOpenUs(int64_t t0_us)  { return t0_us - (int64_t) kArmLeadUs; }
constexpr int64_t windowCloseUs(int64_t t0_us) { return windowOpenUs(t0_us) + kWindowUs; }

// True if a frame whose T0 lands `err_us` away from the expected T0 is caught.
// Positive err = the frame is LATE relative to the node's expectation.
constexpr bool phaseErrorIsCaught(int32_t err_us)
{
    return err_us >= -(int32_t) kGuardUs && err_us <= (int32_t) kGuardUs;
}

// --- Slot instants --------------------------------------------------------

constexpr int64_t t0ForSlot(int64_t anchor_us, uint32_t round, uint32_t slot)
{
    // round is deliberately widened before scaling: at 1.5 s a uint32 round
    // counter wraps after 204 years, but the PRODUCT overflows 32 bits after
    // 48 minutes, which is the mistake this cast exists to prevent.
    return anchor_us
         + (int64_t) round * (int64_t) kRoundUs
         + (int64_t) slot * (int64_t) kSlotPitchUs;
}

// --- How many nodes the hub can serve per round ---------------------------
//
// A transmission is wider than a window, so serving node k also covers its
// neighbours' windows. The hub occupies, relative to T0_k:
//
//   start = -kPreambleToT0Us
//   end   = t0ToRxDoneUs(downlink) + turnaround + timeOnAirUs(ack)
//
// `turnaround_us` is the node's DRAIN + build time: FIFO read, protobuf unpack,
// AEAD decrypt and three queue hops on a CPU that scales down to 40 MHz. It has
// NEVER BEEN MEASURED (implementation-plan.md section 12.7 / test-plan.md
// HW-7), so it is a parameter here rather than a constant. Every function below
// takes it explicitly for that reason.
static constexpr uint32_t kAckPayloadBytes = 25;

constexpr uint32_t hubOccupancyEndUs(uint32_t downlink_len, uint32_t turnaround_us)
{
    return t0ToRxDoneUs(downlink_len) + turnaround_us + timeOnAirUs(kAckPayloadBytes);
}

// The next slot offset whose window opens clear of that occupancy.
constexpr uint32_t nextServableSlotDelta(uint32_t downlink_len, uint32_t turnaround_us)
{
    const uint32_t need = hubOccupancyEndUs(downlink_len, turnaround_us) + kArmLeadUs;
    return (need + kSlotPitchUs - 1) / kSlotPitchUs;   // ceil
}

constexpr uint32_t nodesPerRound(uint32_t downlink_len, uint32_t turnaround_us)
{
    const uint32_t d = nextServableSlotDelta(downlink_len, turnaround_us);
    return d == 0 ? kSlotCount : kSlotCount / d;
}

// The largest turnaround for which `delta` still holds. This is the sensitivity
// that decides whether the geometry survives HW-7's answer.
constexpr uint32_t maxTurnaroundForDelta(uint32_t downlink_len, uint32_t delta)
{
    const int64_t budget = (int64_t) delta * kSlotPitchUs
                         - (int64_t) kArmLeadUs
                         - (int64_t) timeOnAirUs(kAckPayloadBytes)
                         - (int64_t) t0ToRxDoneUs(downlink_len);
    return budget < 0 ? 0u : (uint32_t) budget;
}

// --- The beacon -----------------------------------------------------------
//
// The 32 private windows are at 32 different phases, so one broadcast cannot
// reach them all: the beacon needs its own instant that every node opens.
//
// It is NOT covered by the servable-slot rule above, which governs unicast
// traffic only. Placed at slot b's T0, a 45 B beacon runs from -3.136 to
// +30.720 ms and covers slot b+1's window opening at +29.659 -- blinding the
// SAME node on every beacon round, forever. That failure would present as
// "node b+1 is unreliable" and would never be attributed to the beacon.
static constexpr uint32_t kBeaconPayloadBytes = 45;

constexpr uint32_t beaconClearSlots()
{
    return (timeOnAirUs(kBeaconPayloadBytes) + kSlotPitchUs - 1) / kSlotPitchUs;
}

// How long the node can coast on a phase measurement before the guard band is
// spent, at a given clock error. The design operates at HALF these ceilings,
// leaving margin for ppm uncertainty and for one lost beacon (which doubles the
// elapsed time since sync).
constexpr uint32_t maxResyncIntervalS(uint32_t ppm)
{
    return ppm == 0 ? 0xFFFFFFFFu : kGuardUs / ppm;
}

// --- Why there is exactly one window per round ----------------------------
//
// A second window per round is not a tuning choice; at 32 slots it is
// impossible. The primary windows tile the round leaving gaps of
// (pitch - width) = 17.435 ms, and a window is 29.44 ms wide, so a second
// window cannot fit in any gap NO MATTER WHERE IT IS PLACED. It will overlap
// some node's primary window.
//
// The specific proposal that prompted this -- two windows 750 ms apart -- is
// worse than merely overlapping: 750000 / 46875 = 16 EXACTLY, so every node's
// second window would land precisely on node (k+16)'s primary T0, for all 32
// nodes at once. A systematic collision, not a probabilistic one.
static constexpr uint32_t kInterWindowGapUs = kSlotPitchUs - kWindowUs;

constexpr bool secondWindowFitsInGap() { return kWindowUs <= kInterWindowGapUs; }

static constexpr uint32_t kWindowsPerRound = 1;

// --- Compile-time pins ----------------------------------------------------
static_assert(kSlotPitchUs == 46875, "32 slots in 1500 ms");
static_assert(kWindowUs == 29440,    "115 symbols at 256 us");
static_assert(kGuardUs == 14080,     "(29440 - 1280) / 2");
static_assert(kArmLeadUs == 17216,   "3136 + 14080");
static_assert(kInterWindowGapUs == 17435, "clear time between adjacent windows");
static_assert(!secondWindowFitsInGap(), "a second window cannot be disjoint at 32 slots");
static_assert(beaconClearSlots() == 1, "the beacon must leave one slot clear");

}  // namespace timedgrid
