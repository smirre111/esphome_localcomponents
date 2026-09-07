#pragma once

#include <stdint.h>

#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// GridState — the node's copy of the timed-window grid (B3).
//
// The anchor is SOLVED LOCALLY, never transferred. The hub's esp_timer and the
// node's are unrelated clocks, so a hub-absolute anchor is meaningless here.
// GridSync instead declares its own position — "my T0 is round r, slot s" — and
// the node, which already recovers T0 from RxDone, solves:
//
//   anchor = T0_measured - r*roundUs - s*pitchUs
//
// The result is the same grid expressed in the node's own time. No clock
// crosses the link; only a position does.
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace gridstate
{

// Why a published grid was refused. Reported, never silently ignored: a node
// that quietly declines a grid looks exactly like one that never heard it.
enum class Refusal : uint8_t {
    None = 0,
    SlotCountMismatch,   // hub and node disagree about grid depth
    PitchMismatch,       // ... or about the slot pitch
    RoundMismatch,       // ... or about the round
    SlotOutOfRange,      // slotIndex >= slotCount
    TxSlotOutOfRange,    // the frame claims a position off its own grid
    BeaconSlotOutOfRange,
    SweepOffsetNotAllowed,   // non-zero armOffsetUs on a node that is not a bench unit
};

struct Params
{
    uint32_t slot_index{0};
    uint32_t slot_count{timedgrid::kSlotCount};
    uint32_t round_us{timedgrid::kRoundUs};
    uint32_t pitch_us{timedgrid::kSlotPitchUs};
    uint32_t beacon_slot{0};
    uint32_t beacon_every_rounds{0};
    uint32_t sym_timeout{timedgrid::kSymbolTimeoutSymbols};
    uint32_t resync_max_s{704};
    uint32_t ul_offset_us{0};
    // HW-2 sweep offset. Zero in every normal configuration; non-zero
    // deliberately mis-arms the window and is refused off the bench.
    int32_t  arm_offset_us{0};
};

// Agreement check, run BEFORE anything is adopted.
//
// A hub and node compiled against different TimedGrid.h constants would each
// believe in a different pitch and quietly miss every window — a failure that
// looks like a dead radio. The geometry is carried on the wire precisely so
// this check is possible; refusing is much better than half-adopting.
constexpr Refusal validate(const Params &p, uint32_t tx_slot, bool is_bench_node = true)
{
    // Checked FIRST: a sweep offset is the one parameter here that
    // deliberately breaks reception, so refusing it must not depend on the
    // rest of the grid being agreeable.
    if (p.arm_offset_us != 0 && !is_bench_node)     return Refusal::SweepOffsetNotAllowed;
    if (p.slot_count != timedgrid::kSlotCount)      return Refusal::SlotCountMismatch;
    if (p.round_us   != timedgrid::kRoundUs)        return Refusal::RoundMismatch;
    if (p.pitch_us   != timedgrid::kSlotPitchUs)    return Refusal::PitchMismatch;
    if (p.slot_index >= p.slot_count)               return Refusal::SlotOutOfRange;
    if (tx_slot      >= p.slot_count)               return Refusal::TxSlotOutOfRange;
    if (p.beacon_every_rounds != 0 &&
        p.beacon_slot >= p.slot_count)              return Refusal::BeaconSlotOutOfRange;
    return Refusal::None;
}

// Solve for the local anchor from a frame whose grid position the hub declared.
constexpr int64_t solveAnchorUs(int64_t t0_measured_us, uint32_t tx_round,
                                uint32_t tx_slot, const Params &p)
{
    return t0_measured_us
         - (int64_t) tx_round * (int64_t) p.round_us
         - (int64_t) tx_slot  * (int64_t) p.pitch_us;
}

struct State
{
    bool     active{false};
    Params   params{};
    int64_t  anchor_us{0};
    uint32_t last_round{0};

    void clear() { *this = State{}; }
};

// When the receiver opens for a given mark: T0 - (T_pre + G), plus the HW-2
// sweep offset. The offset is added rather than folded into the arm lead so
// that a sweep never silently becomes the node's idea of correct.
constexpr int64_t armInstantUs(const State &st, int64_t t0_us)
{
    return t0_us - (int64_t) timedgrid::kArmLeadUs
                 + (int64_t) st.params.arm_offset_us;
}

// This node's T0 for a given round.
constexpr int64_t t0ForRound(const State &st, uint32_t round)
{
    return st.anchor_us
         + (int64_t) round * (int64_t) st.params.round_us
         + (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us;
}

// The first T0 at or after `now`. Same explicit negative branch as the hub's
// nextT0ForSlotUs: integer division truncates towards zero, so the usual
// (d + round - 1)/round rounds the wrong way for a `now` before the first T0
// and would hand back an instant in the past.
constexpr int64_t nextT0Us(const State &st, int64_t now_us)
{
    if (!st.active) return now_us;
    const int64_t base  = t0ForRound(st, 0);
    const int64_t round = (int64_t) st.params.round_us;
    const int64_t delta = now_us - base;
    if (delta <= 0) return base;
    return base + ((delta + round - 1) / round) * round;
}

// Is `round` a beacon round? Round 0 counts, so a node that has just adopted a
// grid does not wait a full interval before its first beacon window.
constexpr bool isBeaconRound(const State &st, uint32_t round)
{
    return st.params.beacon_every_rounds != 0 &&
           (round % st.params.beacon_every_rounds) == 0;
}

}  // namespace gridstate
