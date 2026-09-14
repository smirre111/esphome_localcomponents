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

// --- MAC-0 clock discipline: the node rate against the hub ------------------
//
// MEASURED 2026-09-13: under the production power profile (auto light sleep)
// node 2 counts +60 ppm against the hub; with sleep disabled, +9. Every function
// below used to step the grid in whole hub-clock rounds, which is exact only if
// the two clocks agree. At +60 ppm the node leaves the +/-14 080 us guard in
// about 235 s, inside one 5.8 min beacon interval, and never comes back.
//
// rate_ppb is how many MORE microseconds the node counts per hub microsecond, in
// parts per billion: positive when a hub-clock span looks longer on the node.
// That is the sign of the run-scoped fit (+60 ppm) and of the positive, growing
// phase error seen on the bench. A span measured on the hub clock is stretched
// onto the node clock by exactly that factor; with rate_ppb == 0 every result
// below is bit-identical to the fixed-round arithmetic it replaces.
constexpr int64_t kRateDen = 1000000000LL;

// The same solve for a node that already holds a rate: the declared position is
// a hub-clock span, so it is stretched before it is subtracted.
constexpr int64_t solveAnchorUs(int64_t t0_measured_us, uint32_t tx_round,
                                uint32_t tx_slot, const Params &p, int32_t rate_ppb)
{
    const int64_t span = (int64_t) tx_round * (int64_t) p.round_us
                       + (int64_t) tx_slot  * (int64_t) p.pitch_us;
    return t0_measured_us - (span + (span * (int64_t) rate_ppb) / kRateDen);
}

struct State
{
    bool     active{false};
    Params   params{};
    int64_t  anchor_us{0};
    uint32_t last_round{0};
    // The node rate against the hub, ppb (see kRateDen). 0 until something has
    // measured it, which reproduces the fixed-round grid exactly.
    int32_t  rate_ppb{0};

    void clear() { *this = State{}; }
};

// A span on the hub clock, as the node clock counts it.
//
// Overflow: a span stays below about 2.6e12 us for a month of uptime; times a
// rate clamped to +/-200 ppm (2e5 ppb) that is 5.2e17, inside int64.
constexpr int64_t stretchUs(const State &st, int64_t hub_span_us)
{
    return hub_span_us + (hub_span_us * (int64_t) st.rate_ppb) / kRateDen;
}

// The inverse, to first order in the rate. Used ONLY to seed the exact searches
// below, never as an answer: its error is rate squared and the searches remove it.
constexpr int64_t unstretchUs(const State &st, int64_t node_span_us)
{
    return node_span_us - (node_span_us * (int64_t) st.rate_ppb) / kRateDen;
}

// When the receiver opens for a given mark: T0 - (T_pre + G), plus the HW-2
// sweep offset. The offset is added rather than folded into the arm lead so
// that a sweep never silently becomes the node's idea of correct.
constexpr int64_t armInstantUs(const State &st, int64_t t0_us)
{
    return t0_us - (int64_t) timedgrid::kArmLeadUs
                 + (int64_t) st.params.arm_offset_us;
}


// --- The hub's declared fire instant (LoraHeader fireRound / fireOffsetUs) ---
//
// Every copy the hub sends states where its own T0 is on the hub grid: `round`
// rounds after the hub's anchor, `offset_us` into that round. On this node's
// grid that is the same span from the node's anchor, stretched by the learned
// rate. Nothing about slots or burst copies enters it: the hub already said
// exactly where THIS copy is.
constexpr int64_t t0ForHubInstantUs(const State &st, uint32_t round, uint32_t offset_us)
{
    return st.anchor_us
         + stretchUs(st, (int64_t) round * (int64_t) st.params.round_us + (int64_t) offset_us);
}

// The anchor that puts a copy measured at `t0_measured_us` on its declared
// instant. The GridSync solve for a stamped copy: unlike solveAnchorUs it trusts
// the instant the frame really left, not the mark it was placed on, so a late
// GridSync no longer moves every mark (measured 2026-09-14: -322 ms, -10 ms).
constexpr int64_t solveAnchorFromHubInstantUs(int64_t t0_measured_us, uint32_t round,
                                              uint32_t offset_us, const Params &p,
                                              int32_t rate_ppb)
{
    const int64_t span = (int64_t) round * (int64_t) p.round_us + (int64_t) offset_us;
    return t0_measured_us - (span + (span * (int64_t) rate_ppb) / kRateDen);
}

// This node's T0 for a given round.
constexpr int64_t t0ForRound(const State &st, uint32_t round)
{
    return st.anchor_us
         + stretchUs(st, (int64_t) round * (int64_t) st.params.round_us
                       + (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us);
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
    if (now_us <= base || round == 0) return base;
    // Seeded from the nominal answer, then made exact: the smallest round whose
    // T0 is at or after now. With rate_ppb == 0 the seed is already exact and
    // neither loop runs.
    int64_t r = (unstretchUs(st, now_us - base) + round - 1) / round;
    if (r < 0) r = 0;
    while (r > 0 && t0ForRound(st, (uint32_t) (r - 1)) >= now_us) --r;
    while (t0ForRound(st, (uint32_t) r) < now_us) ++r;
    return t0ForRound(st, (uint32_t) r);
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
    if (st.params.round_us == 0) return 0;
    const int64_t base = t0ForRound(st, 0);
    if (t0_us < base) return 0;
    const int64_t round = (int64_t) st.params.round_us;
    // Seeded, then exact: the largest round whose T0 is at or before t0_us.
    int64_t r = unstretchUs(st, t0_us - base) / round;
    if (r < 0) r = 0;
    while (r > 0 && t0ForRound(st, (uint32_t) r) > t0_us) --r;
    while (t0ForRound(st, (uint32_t) (r + 1)) <= t0_us) ++r;
    return (uint32_t) r;
}

// The BEACON's T0 in a given round — the beacon slot's phase, not this node's.
constexpr int64_t beaconT0ForRound(const State &st, uint32_t round)
{
    return st.anchor_us
         + stretchUs(st, (int64_t) round * (int64_t) st.params.round_us
                       + (int64_t) st.params.beacon_slot * (int64_t) st.params.pitch_us);
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
    const int64_t every  = (int64_t) st.params.beacon_every_rounds;
    // Seeded, then exact: the first beacon round whose T0 is at or after now.
    int64_t k = (unstretchUs(st, now_us - base) + stride - 1) / stride;
    if (k < 0) k = 0;
    while (k > 0 && beaconT0ForRound(st, (uint32_t) ((k - 1) * every)) >= now_us) --k;
    while (beaconT0ForRound(st, (uint32_t) (k * every)) < now_us) ++k;
    return beaconT0ForRound(st, (uint32_t) (k * every));
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

// Where to look for the next window, given the T0 of the window that has
// already OPENED (0 = none).
//
// Measured 2026-09-14 on node 2 (fw 1.0.68, first Mode B promotion): the
// receive task comes round a few ms after opening a window, while that window
// is still listening and before its T0. nextT0Us(now) is then the SAME mark, so
// it was armed again at 1 us — the task woke at once and blocked on the radio
// mutex until the first window closed, then opened a second window after the
// mark had passed. The node heard none of ~120 marks the hub placed. Class A
// had the identical defect and classa::armPlan fixed it there.
//
// Half a slot pitch past the opened T0: windows are at least one pitch apart,
// and a beacon re-anchor moves a predicted T0 by at most the guard (14 080 us),
// less than half a pitch (23 437 us) — so the opened window can neither be found
// again nor hide the next one.
constexpr int64_t nextWindowSearchFromUs(int64_t now_us, int64_t opened_t0_us)
{
    if (opened_t0_us == 0) return now_us;
    const int64_t after = opened_t0_us + (int64_t) timedgrid::kSlotPitchUs / 2;
    return (after > now_us) ? after : now_us;
}

// armDelayUs for whichever window comes next, reporting which one it is: the
// caller has to know, because a beacon window that closes empty is not a missed
// MARK and must not feed the demotion counter.
//
// `opened_t0_us` is the window that has already opened (see
// nextWindowSearchFromUs); `t0_out`, when given, receives the T0 aimed at, so
// the caller can record it once that window opens. The delay is still measured
// from `now_us`.
constexpr int64_t nextWindowArmDelayUs(const State &st, int64_t now_us,
                                       int64_t lead_us, bool skip_own,
                                       WindowKind &kind_out,
                                       int64_t opened_t0_us = 0,
                                       int64_t *t0_out = nullptr)
{
    if (!st.active)
    {
        kind_out = WindowKind::Own;
        if (t0_out != nullptr) *t0_out = 0;
        return 1;
    }
    const NextWindow w =
        nextWindow(st, nextWindowSearchFromUs(now_us, opened_t0_us), skip_own);
    kind_out = w.kind;
    if (t0_out != nullptr) *t0_out = w.t0_us;
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
    // The published offset is a hub-clock span like the round, so it is
    // stretched with it: uplink mark n sits at anchor + stretch(n * round + slot
    // offset + ul_offset). With rate_ppb == 0 this is exactly base + n * round.
    const int64_t round  = (int64_t) st.params.round_us;
    const int64_t offset = (int64_t) st.params.slot_index * (int64_t) st.params.pitch_us
                         + (int64_t) st.params.ul_offset_us;
    const int64_t target = now_us + lead_us;
    const int64_t base   = st.anchor_us + stretchUs(st, offset);
    int64_t n = 0;
    if (target > base)
    {
        n = (unstretchUs(st, target - base) + round - 1) / round;
        if (n < 0) n = 0;
        while (n > 0 && st.anchor_us + stretchUs(st, (n - 1) * round + offset) >= target) --n;
        while (st.anchor_us + stretchUs(st, n * round + offset) < target) ++n;
    }
    const int64_t t0    = st.anchor_us + stretchUs(st, n * round + offset);
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
// The rule for a PHASE SAMPLE that feeds promotion: inside the guard band.
//
// REFUTED 2026-09-13, and only the second half of the old reasoning: it said a
// larger error "is not drift — one round at +/-20 ppm is 30 us, and a whole
// resyncMaxS of it still fits inside G". Measured under the production power
// profile the node clock runs +60 ppm against the hub, so a beacon interval
// (5.8 min) accumulates ~21 ms — outside G, and still drift. A guard-only rule
// discarded exactly the beacons that could have corrected it.
//
// So there are now two bounds with two jobs. This one still gates the sample
// promotion is judged on. beaconErrLearnable() below gates re-anchoring and rate
// learning, with a capture wide enough to hold a beacon interval of real drift.
constexpr bool reanchorIsSane(int64_t err_us, uint32_t guard_us)
{
    return err_us <= (int64_t) guard_us && err_us >= -(int64_t) guard_us;
}

// The capture for LEARNING and re-anchoring from a beacon: half a slot pitch.
//
// Wider than the guard because drift between beacons can exceed the guard
// (measured: ~21 ms per 5.8 min at +60 ppm), and a node must be able to
// correct from exactly those beacons. No wider than half a pitch, because
// beyond that the frame is closer to a neighbouring slot than to our own
// mark and the error no longer identifies which mark it belongs to.
constexpr bool beaconErrLearnable(int64_t err_us, uint32_t pitch_us)
{
    const int64_t half = (int64_t) pitch_us / 2;
    return err_us <= half && err_us >= -half;
}

// The residual rate one beacon reveals, ppb: its error over the hub-clock span
// since the last accepted beacon, whose re-anchor had zeroed the error. 0 when
// the span is too short to tell drift from timestamp jitter.
constexpr int32_t kRateMaxPpb = 200000;              // +/-200 ppm clamp
constexpr int64_t kRateMinSpanUs = 60LL * 1000000;   // one minute
constexpr int32_t residualRatePpb(int64_t err_us, int64_t hub_span_us)
{
    if (hub_span_us < kRateMinSpanUs) return 0;
    return (int32_t) ((err_us * kRateDen) / hub_span_us);
}

constexpr int32_t clampRatePpb(int64_t ppb)
{
    if (ppb >  kRateMaxPpb) return  kRateMaxPpb;
    if (ppb < -kRateMaxPpb) return -kRateMaxPpb;
    return (int32_t) ppb;
}

}  // namespace gridstate
