#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// TimedModePolicy — when a node may be in Mode B, and when the hub may send a
// single copy instead of a 17-copy burst.
//
// THE ASYMMETRY RULE, which is the whole safety argument:
//
//   1. the node may drop to Mode A UNILATERALLY, at any moment;
//   2. the hub may send single-shot ONLY with positive, recent confirmation
//      that the node is in a timed window;
//   3. any hub uncertainty -> burst (reboot, session change, missed beacon,
//      stale confirmation, unknown firmware);
//   4. a single shot that goes unacked is retried AS A BURST, immediately.
//
// The dangerous combination is the hub sending one copy while the node is back
// in Mode A: one 29 ms window per 500 ms gives ~5.8 % delivery.
//
// A NOTE ON HOW STRONG THIS GUARANTEE ACTUALLY IS.
//
// The design document says that combination is "unreachable by construction".
// It is not, and writing the exhaustive test is what showed it: the hub's
// confirmation is a DELAYED OBSERVATION of the node's state. The node can
// demote unilaterally and instantly — rule 1, which is itself a safety
// property — while the hub still holds a confirmation that was true when it
// was made. No predicate over the hub's own state can rule that out, because
// the hub does not have the node's state; it has an echo of it.
//
// What IS guaranteed, and what rule 4 is really for, is BOUNDED EXPOSURE: at
// most ONE single-shot frame can be sent into a node that has demoted, after
// which the policy reverts to burst until fresh confirmation arrives. That is
// the property asserted in the tests, and it is the honest one. The cost of
// being wrong is one lost frame and one retry, not a lost node.
//
// Dependency-free. See implementation-plan.md section 4.6, test-plan.md 5.3.
// ---------------------------------------------------------------------------

namespace timedmode
{

// Where the node's RTC slow clock actually comes from. Mode B is gated on the
// external 32.768 kHz crystal: a node that has fallen back to the internal RC
// oscillator (~5 %) cannot hold phase between beacons and must stay in Mode A
// VISIBLY, as a reported state, rather than silently failing to hear anything.
enum class RtcSlowSrc : uint8_t { Unknown = 0, InternalRc = 1, Crystal = 2 };

enum class Mode : uint8_t { A = 0, B = 1 };

// Ordered by how actionable they are: the first reason a caller can do
// something about comes first.
enum class Demotion : uint8_t {
    None = 0,
    GridDisabled,      // the hub withdrew the grid (incl. its startup demote)
    BadClockSource,    // rtcSlowSrc is not the crystal
    MissedMarks,       // K consecutive marks with no frame ADDRESSED to me
    SyncStale,         // no addressed frame for resyncMaxS
    NoPhase,           // never measured, or the measurement is out of guard
    NotConfirmed,      // not enough in-slot uplinks observed yet
    RecentlyDemoted,   // anti-flap hold
};

// K consecutive missed marks before demoting.
static constexpr uint32_t kMaxMissedMarks = 3;

// In-slot uplinks the hub must have OBSERVED before the node is considered
// promoted. A node claiming readiness is not evidence: a beacon saying "I am
// ready" says nothing about where its window actually landed.
//
// STILL TRUE, AND NO LONGER THE HUB'S PROMOTION CRITERION. See
// kPromotionPhaseSamples for what replaced it and why. This constant still
// governs the NODE's own view (NodeState::in_slot_uplinks) and the hub still
// counts placement as a diagnostic; it simply no longer gates single-shot.
static constexpr uint32_t kPromotionUplinks = 3;

// --- What the hub promotes on -------------------------------------------
//
// The question single-shot actually turns on is: WILL THIS NODE'S RECEIVE
// WINDOW BE OPEN WHEN MY ONE COPY ARRIVES? In-slot uplink placement was a
// proxy for that, and a poor one — it is produced by the node's TRANSMIT path,
// which is governed by CAD, a burst-end deferral and a random pre-transmit
// backoff, none of which has anything to do with when the node ARMS. Measured
// against a 29 ms transmit quantum and a 14 ms tolerance, the proxy was not
// merely noisy, it was unsatisfiable: single-shot could never engage at all.
//
// The node's own phase statistics answer the real question directly. phaseErr
// is the node measuring where the HUB's frames landed relative to the marks it
// armed for — an observation of this link, by the end that has to hear it, and
// it rides an ENCRYPTED beacon, so it is authenticated evidence rather than an
// unauthenticated claim.
//
// This is a deliberate departure from "a node claiming readiness is not
// evidence", and the distinction it rests on is worth stating: the node is not
// asserting that it is ready. It is reporting a MEASUREMENT of the hub's own
// transmissions. The failure mode that rule guarded against — a node that says
// it is fine and then hears nothing — shows up here as phase error or spread
// outside the guard, which is exactly the thing being tested.
//
// Rule 4 is unchanged and is still what bounds the damage: a single shot that
// goes unacked reverts to burst immediately, so being wrong costs one frame.
//
// Samples required before the report means anything. A phase fit over one or
// two frames says nothing about spread, which is the half that catches a
// bimodal distribution (two clusters a slot pitch apart average to something
// innocent).
static constexpr uint32_t kPromotionPhaseSamples = 8;

// No promotion within 10 minutes of a demotion. Without this a node at
// beacon_interval_s = 0 flaps several times a day.
static constexpr uint32_t kRepromotionHoldS = 600;

// How stale the hub's confirmation may be before single-shot is withdrawn is
// NOT a constant here: it is the resyncMaxS the hub itself published, passed
// into txPolicyFor.
//
// That is the same interval the NODE uses to decide its phase has gone stale —
// derived from the guard band and the crystal spec (maxResyncIntervalS), and
// the interval after which the node demotes itself for want of an addressed
// frame. Using anything else would have the two ends disagree about when the
// node is still in Mode B, and the hub is the one that would be wrong: it
// would keep sending single copies to a node that had already gone back to
// sweeping a free-running window.
//
// A 60-second constant stood here briefly and was worse than wrong, it was
// inert: the report rides uplinks the node already sends, so on a fleet at 3.5
// commands/day nothing is ever 60 seconds old and single-shot could not engage
// at all. Passing 0 keeps the fail-closed direction — an unpublished interval
// means no promotion, not unlimited trust.

// --- Node side ------------------------------------------------------------

struct NodeState
{
    RtcSlowSrc rtc_src                 = RtcSlowSrc::Unknown;
    bool       grid_enabled            = false;  // hub published a grid
    bool       phase_valid             = false;  // T0 has been measured
    int32_t    phase_err_us            = 0;      // measured - predicted
    // Consecutive marks at which NO FRAME ADDRESSED TO ME arrived. It must not
    // count "nothing received": a window walked through by another node's burst
    // is not empty, and keying on silence would let ordinary Mode A traffic
    // demote the very node this policy is protecting.
    uint32_t   consecutive_missed_marks = 0;
    uint32_t   s_since_addressed_frame  = 0;
    uint32_t   in_slot_uplinks          = 0;     // consecutive, hub-confirmed
    uint32_t   s_since_demotion         = 0xFFFFFFFF;
};

// Guard half-width the phase error must fall inside. Passed in rather than
// taken from TimedGrid.h so this header stays independent of the grid geometry
// (and so a widened symbol timeout is expressible).
constexpr Demotion demotionReason(const NodeState &s, uint32_t resync_max_s,
                                  uint32_t guard_us)
{
    if (!s.grid_enabled)                                    return Demotion::GridDisabled;
    if (s.rtc_src != RtcSlowSrc::Crystal)                   return Demotion::BadClockSource;
    if (s.consecutive_missed_marks >= kMaxMissedMarks)      return Demotion::MissedMarks;
    if (s.s_since_addressed_frame > resync_max_s)           return Demotion::SyncStale;
    if (!s.phase_valid)                                     return Demotion::NoPhase;
    if (s.phase_err_us > (int32_t) guard_us ||
        s.phase_err_us < -(int32_t) guard_us)               return Demotion::NoPhase;
    if (s.in_slot_uplinks < kPromotionUplinks)              return Demotion::NotConfirmed;
    if (s.s_since_demotion < kRepromotionHoldS)             return Demotion::RecentlyDemoted;
    return Demotion::None;
}

constexpr Mode modeFor(const NodeState &s, uint32_t resync_max_s, uint32_t guard_us)
{
    return demotionReason(s, resync_max_s, guard_us) == Demotion::None ? Mode::B : Mode::A;
}

// --- Hub side -------------------------------------------------------------

enum class TxPolicy : uint8_t { Burst = 0, SingleShot = 1 };

struct HubBelief
{
    bool     grid_enabled          = false;
    // Kept and still maintained, as a diagnostic and because it is the honest
    // record of what the hub OBSERVED. It no longer gates single-shot — see
    // kPromotionPhaseSamples.
    uint32_t in_slot_acks          = 0;  // consecutive acks observed in slot
    // Age of the beacon carrying the phase report below.
    uint32_t confirmation_age_s    = 0xFFFFFFFF;

    // The node's own phase measurement, from its last DECRYPTED beacon. This
    // is what single-shot is promoted on.
    bool       phase_reported  = false;
    int32_t    phase_err_us    = 0;
    int32_t    phase_spread_us = 0;
    uint32_t   phase_samples   = 0;
    // Samples the node itself scored as outside the guard band. Mean and spread
    // can both pass while individual samples miss: a tight cluster offset by
    // most of a guard band has a small spread and a mean that is still inside,
    // and every frame in it lands near the edge of the window. The node already
    // counts these (phase::Stats::outside_guard) and its own phaseTrustworthy()
    // requires zero, so requiring zero here makes the two ends agree about what
    // "in phase" means instead of approximating it.
    uint32_t   phase_outside_guard = 0;
    // Mode B is gated on the external crystal at both ends: a node on the
    // internal RC (~5 %) cannot hold phase between beacons, whatever its last
    // report said.
    RtcSlowSrc rtc_src         = RtcSlowSrc::Unknown;
    bool     rebooted_since_confirm = true;
    bool     session_changed        = false;
    bool     beacon_missed          = false;
    bool     firmware_known         = false;
    // Set the moment a single shot goes unacked, cleared by fresh confirmation.
    // Rule 4: this is what bounds the exposure to one frame.
    bool     single_shot_unacked    = false;
};

// guard_us is passed in for the same reason demotionReason takes it: this
// header stays independent of the grid geometry.
// max_age_s is the hub's published resyncMaxS — see the banner above where the
// constant used to be. Zero refuses everything, which is the safe direction for
// a caller that has published no interval.
// WHY a node is still on bursts, in the order the reasons are tested.
//
// U-1: txPolicyFor answered only Burst-or-SingleShot, so a node stuck on
// bursts — paying seventeen copies for every frame, which is the entire cost
// Mode B exists to remove — gave an operator nothing to act on. The single
// boolean is not the diagnostic; WHICH of fourteen conditions failed is.
//
// This mirrors the node's own demotionReason() beside modeFor(), and for the
// same reason: one list of reasons to fall back, tested in one order, with the
// yes/no answer DERIVED from it rather than restated. Two copies of the ladder
// would drift and the drift would be silent — the hub would burst while
// reporting a reason that says it should not.
//
// Numbered explicitly because the value is published to Home Assistant, so the
// numbers are a wire format: append, never renumber.
enum class TxRefusal : uint8_t {
    None              = 0,   // single shot
    GridDisabled      = 1,   // no grid published to this node
    RebootedSinceConfirm = 2,
    SessionChanged    = 3,
    BeaconMissed      = 4,
    FirmwareUnknown   = 5,   // no decrypted beacon has carried a version
    SingleShotUnacked = 6,   // Rule 4's one-frame exposure
    NoPhaseReport     = 7,
    BadClockSource    = 8,   // not the external crystal
    TooFewSamples     = 9,
    PhaseOutOfGuard   = 10,  // the mean is outside the guard band
    SpreadTooWide     = 11,  // bimodal: a mean can pass while samples miss
    SamplesOutOfGuard = 12,  // the node's own count, which must be zero
    NoPublishedMaxAge = 13,  // the hub published no resyncMaxS: fail closed
    ConfirmationStale = 14,
};

constexpr TxRefusal txRefusalFor(const HubBelief &b, uint32_t guard_us,
                                 uint32_t max_age_s)
{
    if (!b.grid_enabled)                            return TxRefusal::GridDisabled;
    if (b.rebooted_since_confirm)                   return TxRefusal::RebootedSinceConfirm;
    if (b.session_changed)                          return TxRefusal::SessionChanged;
    if (b.beacon_missed)                            return TxRefusal::BeaconMissed;
    if (!b.firmware_known)                          return TxRefusal::FirmwareUnknown;
    if (b.single_shot_unacked)                      return TxRefusal::SingleShotUnacked;

    // The node's phase report, and every way of not having one.
    if (!b.phase_reported)                          return TxRefusal::NoPhaseReport;
    if (b.rtc_src != RtcSlowSrc::Crystal)           return TxRefusal::BadClockSource;
    if (b.phase_samples < kPromotionPhaseSamples)   return TxRefusal::TooFewSamples;
    if (b.phase_err_us > (int32_t) guard_us ||
        b.phase_err_us < -(int32_t) guard_us)       return TxRefusal::PhaseOutOfGuard;
    // Spread, not just the mean: two clusters one slot pitch apart average to
    // something innocent, and a node whose window is sometimes right and
    // sometimes a pitch out will drop the single copy on the wrong half.
    if (b.phase_spread_us > (int32_t) guard_us)     return TxRefusal::SpreadTooWide;
    // The node's own test, not an approximation of it.
    if (b.phase_outside_guard != 0)                 return TxRefusal::SamplesOutOfGuard;

    if (max_age_s == 0)                             return TxRefusal::NoPublishedMaxAge;
    if (b.confirmation_age_s > max_age_s)           return TxRefusal::ConfirmationStale;
    return TxRefusal::None;
}

// DERIVED, exactly as modeFor is derived from demotionReason. The ladder above
// is the only copy.
constexpr TxPolicy txPolicyFor(const HubBelief &b, uint32_t guard_us,
                               uint32_t max_age_s)
{
    return txRefusalFor(b, guard_us, max_age_s) == TxRefusal::None
               ? TxPolicy::SingleShot
               : TxPolicy::Burst;
}

// The state the hub must move to the instant a single shot is not acked.
constexpr HubBelief afterUnackedSingleShot(HubBelief b)
{
    b.single_shot_unacked = true;
    return b;
}

}  // namespace timedmode
