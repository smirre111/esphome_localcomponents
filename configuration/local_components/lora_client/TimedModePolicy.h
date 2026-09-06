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
static constexpr uint32_t kPromotionUplinks = 3;

// No promotion within 10 minutes of a demotion. Without this a node at
// beacon_interval_s = 0 flaps several times a day.
static constexpr uint32_t kRepromotionHoldS = 600;

// How stale the hub's confirmation may be before single-shot is withdrawn.
static constexpr uint32_t kMaxConfirmationAgeS = 60;

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
    uint32_t in_slot_acks          = 0;  // consecutive acks observed in slot
    uint32_t confirmation_age_s    = 0xFFFFFFFF;
    bool     rebooted_since_confirm = true;
    bool     session_changed        = false;
    bool     beacon_missed          = false;
    bool     firmware_known         = false;
    // Set the moment a single shot goes unacked, cleared by fresh confirmation.
    // Rule 4: this is what bounds the exposure to one frame.
    bool     single_shot_unacked    = false;
};

constexpr TxPolicy txPolicyFor(const HubBelief &b)
{
    if (!b.grid_enabled)                            return TxPolicy::Burst;
    if (b.rebooted_since_confirm)                   return TxPolicy::Burst;
    if (b.session_changed)                          return TxPolicy::Burst;
    if (b.beacon_missed)                            return TxPolicy::Burst;
    if (!b.firmware_known)                          return TxPolicy::Burst;
    if (b.single_shot_unacked)                      return TxPolicy::Burst;
    if (b.in_slot_acks < kPromotionUplinks)         return TxPolicy::Burst;
    if (b.confirmation_age_s > kMaxConfirmationAgeS) return TxPolicy::Burst;
    return TxPolicy::SingleShot;
}

// The state the hub must move to the instant a single shot is not acked.
constexpr HubBelief afterUnackedSingleShot(HubBelief b)
{
    b.single_shot_unacked = true;
    return b;
}

}  // namespace timedmode
