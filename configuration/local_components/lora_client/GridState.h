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

// How long from `now` until the one-shot that arms the radio should fire.
//
// `lead_us` covers the software between the timer callback and the radio
// actually listening — the semaphore give, the task wake, and the register
// writes that idle the radio, set the DIO mapping and symbol timeout, clear
// interrupts and enter RX single. It belongs to the caller, not to the grid:
// the node measures it (HW-3), the hub has no equivalent.
//
// The result is never negative and never zero. A mark already upon us is armed
// IMMEDIATELY rather than skipped: arming late still catches the frame while
// the lateness is inside the guard band G, and skipping guarantees a miss. An
// inactive grid returns 0 delay for the same reason — the caller should not be
// asking, and stalling is worse than an early arm.
constexpr int64_t armDelayUs(const State &st, int64_t now_us, int64_t lead_us)
{
    if (!st.active) return 1;
    const int64_t d = armInstantUs(st, nextT0Us(st, now_us)) - lead_us - now_us;
    return (d < 1) ? 1 : d;
}

// Is `round` a beacon round? Round 0 counts, so a node that has just adopted a
// grid does not wait a full interval before its first beacon window.
constexpr bool isBeaconRound(const State &st, uint32_t round)
{
    return st.params.beacon_every_rounds != 0 &&
           (round % st.params.beacon_every_rounds) == 0;
}

// --- The beacon window (section 4.4) ---------------------------------------
//
// The 32 private windows sit at 32 different phases, so one broadcast cannot
// reach them all. The beacon has its own instant, in its own slot, on beacon
// rounds only, and every node opens it.

// Which round contains `t0`, measured against THIS node's marks. The inverse of
// t0ForRound, and the number both ends must agree on for "beacon round" to mean
// the same thing — which is why the hub has to declare the round it actually
// transmits in rather than 0.
constexpr uint32_t roundForT0(const State &st, int64_t t0_us)
{
    const int64_t rel = t0_us - st.anchor_us
                      - (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us;
    if (rel < 0 || st.params.round_us == 0) return 0;
    return (uint32_t) (rel / (int64_t) st.params.round_us);
}

// The BEACON's T0 in a given round — the beacon slot's phase, not this node's.
constexpr int64_t beaconT0ForRound(const State &st, uint32_t round)
{
    return st.anchor_us
         + (int64_t) round * (int64_t) st.params.round_us
         + (int64_t) st.params.beacon_slot * (int64_t) st.params.pitch_us;
}

// The first beacon T0 at or after `now`. Same explicit negative branch as
// nextT0Us, and 0 when no cadence has been published — a caller must read that
// as "there is no beacon", never as an instant.
constexpr int64_t nextBeaconT0Us(const State &st, int64_t now_us)
{
    if (!st.active || st.params.beacon_every_rounds == 0) return 0;
    const int64_t stride = (int64_t) st.params.round_us
                         * (int64_t) st.params.beacon_every_rounds;
    const int64_t base   = beaconT0ForRound(st, 0);
    if (now_us <= base) return base;
    return base + ((now_us - base + stride - 1) / stride) * stride;
}

enum class WindowKind : uint8_t { Own, Beacon };

struct NextWindow
{
    int64_t    t0_us{0};
    WindowKind kind{WindowKind::Own};
};

// The next window this node should open.
//
// `skip_own` is section 4.4's Tier 3 permission, and it drops the PRIVATE
// window only. The beacon window is never skipped, for a reason that is easy to
// get backwards: the beacon is the thing that grants and revokes the
// permission, so a node that skipped it too could never learn it has traffic
// waiting, and one lost beacon would strand it instead of expiring the skip.
//
// With no beacon cadence published this is exactly the old behaviour — the
// node's own next mark.
constexpr NextWindow nextWindow(const State &st, int64_t now_us, bool skip_own)
{
    const int64_t own    = nextT0Us(st, now_us);
    const int64_t beacon = nextBeaconT0Us(st, now_us);
    if (beacon == 0)              return NextWindow{own, WindowKind::Own};
    if (skip_own)                 return NextWindow{beacon, WindowKind::Beacon};
    // A tie goes to the beacon: it is the broadcast every node depends on, and
    // the private window comes round again in one round.
    return (beacon <= own) ? NextWindow{beacon, WindowKind::Beacon}
                           : NextWindow{own,    WindowKind::Own};
}

// armDelayUs for whichever window comes next, reporting which one it is: the
// caller has to know, because a beacon window that closes empty is not a missed
// MARK and must not feed the demotion counter.
constexpr int64_t nextWindowArmDelayUs(const State &st, int64_t now_us,
                                       int64_t lead_us, bool skip_own,
                                       WindowKind &kind_out)
{
    if (!st.active) { kind_out = WindowKind::Own; return 1; }
    const NextWindow w = nextWindow(st, now_us, skip_own);
    kind_out = w.kind;
    const int64_t d = armInstantUs(st, w.t0_us) - lead_us - now_us;
    return (d < 1) ? 1 : d;
}

// --- Placing an uplink in this node's slot (section 4.3) --------------------
//
// `ulOffsetUs` has been published in every GridSync since the grid was designed
// and nothing has ever read it: the node replied "as soon as CAD says the
// channel is free", after an unconditional random 29-290 ms backoff. Against a
// 29 ms quantum and the hub's 28.16 ms in-slot band, at most one of ten delays
// could qualify, so section 4.6's in-slot predicate was unreachable and the
// hub's own measurement of where uplinks land meant nothing.
//
// This is the arithmetic that makes the field mean what it says. The node aims
// its uplink at its OWN mark plus the offset — the instant the hub subtracts
// again in noteUplinkPlacement_ — and starts the CAD one lead earlier so the
// CAD, the ramp and the preamble all fit in front of T0.
//
// Two deliberate limits, because this is the node's transmit path and the
// random backoff it replaces is a collision-avoidance measure:
//
//   * It aims at a mark it can still REACH, and `max_wait_us` bounds how long
//     it may wait for one. The bound belongs to the CALLER, not to the grid:
//     the only honest number is the delay this aim replaces, and that lives in
//     the transmit path (LoraInterface::maxUplinkAimWaitUs). Since the marks
//     are one round apart, any bound below a round also guarantees a frame is
//     never held for a later round.
//   * `aimed == false` is not a failure. It is the honest answer whenever there
//     is no grid, no published offset, or no reachable mark, and the caller
//     falls back to exactly the behaviour that shipped before.
struct UplinkAim
{
    bool    aimed{false};
    int64_t cad_start_us{0};   // when to begin CAD
    int64_t t0_us{0};          // where the frame's T0 is aimed
};

// `lead_us` is t0 - cad_start for this frame, i.e. what loratiming::
// cadStartInstantUs() subtracts. It is the caller's because only the caller
// knows its own dispatch cost.
constexpr UplinkAim aimUplink(const State &st, int64_t now_us,
                              int64_t lead_us, int64_t max_wait_us)
{
    if (!st.active || st.params.ul_offset_us == 0 || st.params.round_us == 0)
        return UplinkAim{};

    // This node's marks, shifted by the offset the hub published. Solved
    // directly rather than by stepping: the first n whose CAD start has not
    // already passed. Truncating division rounds towards zero, so the negative
    // branch is explicit for the same reason it is in nextT0Us().
    const int64_t base  = t0ForRound(st, 0) + (int64_t) st.params.ul_offset_us;
    const int64_t round = (int64_t) st.params.round_us;
    const int64_t need  = now_us + lead_us - base;
    const int64_t n     = (need <= 0) ? 0 : ((need + round - 1) / round);
    const int64_t t0    = base + n * round;
    const int64_t start = t0 - lead_us;

    if (start - now_us > max_wait_us)
        return UplinkAim{};      // too far out — send the ordinary way instead
    return UplinkAim{true, start, t0};
}

// --- Re-anchoring from a beacon --------------------------------------------
//
// A beacon is the node's chance to correct the drift accumulated since the last
// frame, which is what lets it hold Mode B for hours rather than for
// resyncMaxS. But it is also a frame the node TIMED before authenticating — the
// arrival instant is captured in the ISR, long before any MAC is checked — so
// an unbounded correction would let anything in radio range walk the anchor
// away. The bound stays even once the beacon carries a fleet-key MAC: a MAC
// proves the sender held the key, and every node in the fleet holds it too.
//
// The rule: correct only by what fits inside the guard band. A larger error is
// not drift — one round at +/-20 ppm is 30 us, and a whole resyncMaxS of it
// still fits inside G — so it is either a foreign frame or a node that has
// already lost the grid. Both are cases for DEMOTING rather than for chasing
// the anchor, which is what the phase tracker's own criteria then do.
constexpr bool reanchorIsSane(int64_t err_us, uint32_t guard_us)
{
    return err_us <= (int64_t) guard_us && err_us >= -(int64_t) guard_us;
}

}  // namespace gridstate
