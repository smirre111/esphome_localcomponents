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
// Numbered explicitly because the value is published to Home Assistant, so the
// numbers are a wire format: append, never renumber. A retired reason keeps its
// value and stops being returned.
enum class Demotion : uint8_t {
    None = 0,
    GridDisabled,      // the hub withdrew the grid (incl. its startup demote)
    BadClockSource,    // rtcSlowSrc is not the crystal
    MissedMarks,       // RETIRED 2026-09-20 — see below
    SyncStale,         // no addressed frame for resyncMaxS: the beacon heartbeat
    NoPhase,           // never measured, or the measurement is out of guard
    NotConfirmed,      // RETIRED 2026-09-14: no longer returned (see demotionReason)
    RecentlyDemoted,   // RETIRED 2026-09-20 — see below
};

// --- Why MissedMarks and RecentlyDemoted are retired (2026-09-20) -----------
//
// MissedMarks COUNTED HUB SILENCE, NOT LINK HEALTH.
//
// The hub publishes pending::allListening() in both GridSync and GridBeacon —
// deliberately, because a cleared bit is a promise it cannot keep for an
// interactive node (lora_tracker.cpp, "ALL LISTENING, deliberately"). So every
// node arms its window every round, and on a fleet at ~3.5 commands/day almost
// every one of those windows is legitimately empty. Three of them — 4.5
// seconds of ordinary quiet — demoted a perfectly healthy node. That is the
// "several times a day" flap kRepromotionHoldS was bought to rate-limit: the
// hold was compensating for a criterion that measured the wrong thing.
//
// THE BEACON HEARTBEAT ALREADY ANSWERS THE REAL QUESTION. SyncStale fires when
// no frame has arrived for the hub's published resyncMaxS — 352 s, one beacon
// interval (349.5 s) plus margin, half the guard/ppm coast limit. And it is
// SUFFICIENT: gridstate::nextWindow derives the mark window and the beacon
// window from ONE anchor, so they drift together. Hearing beacons proves the
// anchor is inside the guard, which proves a mark would be heard if one were
// sent. A node that has genuinely lost its window stops hearing beacons too.
//
// WHAT BOUNDS THE COST OF BEING WRONG: the hub's own half of the asymmetry
// rule. One unacked single shot sets single_shot_unacked (Rule 4) and reverts
// that node to a 17-copy burst spanning 1408 ms, which hits its window whatever
// the aim. The node does not need a 4.5-second criterion of its own.
//
// ACCEPTED COST, stated plainly: a node whose own slot is jammed while the
// beacon slot stays clear now sits in Mode B, deaf, for up to ~5.9 min instead
// of 4.5 s, recovering on the hub's first burst. That trade was made knowingly.
//
// RecentlyDemoted goes with it, not on its own merits: last_demotion_us_ was
// written ONLY by demoteIfMarksMissed_, so with that path gone nothing could
// ever set the hold and the rung was unreachable.
//
// missed_marks_ itself SURVIVES on the node as a WMR diagnostic. It is the
// demotion that is retired, not the measurement.

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

// The 600 s anti-flap hold (kRepromotionHoldS) stood here until 2026-09-20.
// It existed because MissedMarks demoted on hub silence, so a node promoted and
// demoted again within seconds of every command; the hold rate-limited that
// cycle rather than fixing it. With MissedMarks retired the flap has no source,
// and the hold had no writer left — see the banner above the Demotion enum.

// --- Telling the hub the mode changed --------------------------------------
//
// THE PROBLEM. Whether a node is in Mode B reaches the hub only in a
// PhaseReport, and a PhaseReport rides an UPLINK: a wake beacon or a
// CommandAck. An interactive node never sleeps, so it never wakes, so it never
// beacons; with no traffic it has nothing to ack. The hub's belief about a
// promoted, QUIET node is therefore stale indefinitely — and txRefusalFor can
// never clear NoPhaseReport for exactly the node that most deserves single
// shot. Mode B's 17->1 airtime saving is unreachable in the quiet regime Mode B
// exists for.
//
// Measured 2026-09-20 on node 2: 12.6 minutes provably in Mode B — reason 0 on
// its own console, 2.44 % RX, one window per round, 313 consecutive empty marks
// — while the hub reported demotion reason 5 and refusal 7 the whole time.
//
// THE FIX. The node announces a CHANGE, unprompted, with a beacon carrying
// WAKE_MODE_CHANGED. Not periodic: a transition is rare, and a node that is
// not flapping sends one frame and then nothing.
//
// WHY IT NEEDS A FLOOR. An announcement is an UPLINK on a battery node, and
// NoPhase <-> None can still cycle when phase evidence is marginal — node 2 did
// 5 -> 5 -> 0 inside 16 s during the promote-and-hold capture. Announcing every
// transition would reintroduce, as airtime, the very flapping cost the
// MissedMarks retirement removed. So: at most one announcement per interval,
// and the LAST state always wins, because a node that settles must not be left
// described by a transition the hub was never told about.
//
// 60 s: the whole point is that the hub stops being wrong, so the floor is
// short enough to bound staleness at a minute, and long enough that a marginal
// oscillation costs one frame a minute rather than one per transition.
static constexpr uint32_t kModeAnnounceMinS = 60;

// Should the node spend an uplink saying its mode changed?
//
// `changed` is the caller's own edge detection (the reason differs from the one
// last reported to the hub, NOT the one last logged — the log and the wire are
// different audiences). Dependency-free and pure so the floor is host-testable
// rather than a number buried in a timer callback.
constexpr bool shouldAnnounceModeChange(bool changed, uint32_t s_since_announce,
                                        uint32_t min_s = kModeAnnounceMinS)
{
    if (!changed)                    return false;
    // 0xFFFFFFFF is "never announced", which must always be allowed through:
    // the first announcement is the one that ends the hub's default belief.
    if (s_since_announce == 0xFFFFFFFFu) return true;
    return s_since_announce >= min_s;
}

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
    // consecutive_missed_marks and s_since_demotion stood here until
    // 2026-09-20. Both fed rungs that are now retired, and a struct whose only
    // purpose is to feed demotionReason must not carry values nothing reads.
    // The node still COUNTS missed marks (CmdDispatcher::missed_marks_); it
    // simply no longer demotes on them.
    uint32_t   s_since_addressed_frame  = 0;
    uint32_t   in_slot_uplinks          = 0;     // consecutive, hub-confirmed
    // Already in Mode B (the node's previous decision was None), and whether its
    // phase evidence is merely THIN rather than contradicted: no sample outside
    // the guard and the spread inside it, however few samples there are.
    //
    // Measured 2026-09-14 on node 2 (fw 1.0.72): 420 in-guard samples in Mode B,
    // then a beacon learned the clock rate (+9957 ppb), the phase statistics were
    // reset as the new prediction requires, and n = 1 read as NoPhase — a working
    // node demoted itself, and would at every beacon that refines its rate.
    bool       in_mode_b                = false;
    bool       phase_consistent         = false;
};

// Guard half-width the phase error must fall inside. Passed in rather than
// taken from TimedGrid.h so this header stays independent of the grid geometry
// (and so a widened symbol timeout is expressible).
constexpr Demotion demotionReason(const NodeState &s, uint32_t resync_max_s,
                                  uint32_t guard_us)
{
    if (!s.grid_enabled)                                    return Demotion::GridDisabled;
    if (s.rtc_src != RtcSlowSrc::Crystal)                   return Demotion::BadClockSource;
    // The beacon heartbeat, and now the only evidence that this node's window
    // has stopped working. MissedMarks used to sit here and fired on 4.5 s of
    // ordinary hub silence.
    if (s.s_since_addressed_frame > resync_max_s)           return Demotion::SyncStale;
    // Promotion needs trustworthy phase. STAYING in Mode B needs only that
    // nothing contradicts it: a reset (rate update, re-anchor) thins the evidence
    // without saying anything is wrong, and missed marks and staleness above
    // still demote a node whose windows really stopped working.
    if (!s.phase_valid && !(s.in_mode_b && s.phase_consistent))
                                                            return Demotion::NoPhase;
    if (s.phase_err_us > (int32_t) guard_us ||
        s.phase_err_us < -(int32_t) guard_us)               return Demotion::NoPhase;
    // THE NODE IS THE AUTHORITY (decided 2026-09-14). in_slot_uplinks — the
    // hub's count of uplinks it saw land in this node's slot — gated promotion
    // here, and on the bench it could never be earned: a ModeTest sends no
    // uplinks, so the count stayed 0 and no window was ever armed (windows 0/0).
    // Only the node knows whether its own phase error is small enough to open
    // one window per round; phase_valid (phaseTrustworthy: enough samples, all
    // inside the guard, spread inside the guard) and phase_err_us are that
    // evidence. The node reports its decision to the hub (PhaseReport
    // timedRxActive), and the hub follows it — see txRefusalFor.
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
    // The node's own DECISION, reported beside its measurement: it is in Mode B
    // (arming one timed window per round) and, if not, why. Single-shot follows
    // this — the node is the end that knows whether its window will be open.
    bool     node_timed_rx          = false;
    uint32_t node_demotion          = 0;     // Demotion value, for the operator
    bool     rebooted_since_confirm = true;
    bool     session_changed        = false;
    bool     beacon_missed          = false;
    bool     firmware_known         = false;
    // Set the moment a single shot goes unacked, cleared by fresh confirmation.
    // Rule 4: this is what bounds the exposure to one frame.
    bool     single_shot_unacked    = false;

    // OPTIMISTIC SINGLE SHOT — off by default, and the default is the point.
    //
    // When the ONLY thing refusing single shot is that the hub's evidence has
    // gone STALE (NoPhaseReport, ConfirmationStale), the hub may spend one
    // placed copy instead of a 17-copy burst and let §4.6 Rule 4 cover a wrong
    // guess: an unacked single shot is retried as a burst immediately, so being
    // wrong costs exactly one frame.
    //
    //   expected airtime = p x 42 ms + (1 - p) x 757 ms, against 715 ms always
    //   break-even at p = 6 %
    //
    // Measured 2026-09-21: a node genuinely in Mode B hit 198/198 and 194/194
    // windows at WMR 0 ppm, so p is near 1 when the node really is promoted and
    // near the Mode A free-running catch rate (~6 %) when it is not — which is
    // the break-even itself, so the change is close to free even when the hub's
    // optimism is misplaced.
    //
    // THIS IS NOT §4.4's REJECTED KEEPALIVE. That was a PERIODIC unicast at
    // 5.8 min: 274.5 s/day at 32 nodes, 3.4x worse than all-burst. This is
    // demand-driven — it costs nothing while the fleet is idle and adds no
    // periodic traffic at all.
    //
    // It relaxes §4.6's "any hub uncertainty -> burst", which is the safety
    // argument, so it ships behind a switch defaulting OFF and never applies to
    // a rung that is POSITIVE evidence the node moved: rebooted_since_confirm,
    // session_changed, beacon_missed, firmware_known. Those still burst.
    bool     optimistic_single_shot = false;
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
    // 8-12 RETIRED 2026-09-14: the hub no longer re-judges the node's phase
    // measurement; the node's own decision (NodeNotTimed) covers all five.
    // Kept so the published numbers never change meaning.
    BadClockSource    = 8,   // retired
    TooFewSamples     = 9,   // retired
    PhaseOutOfGuard   = 10,  // retired
    SpreadTooWide     = 11,  // retired
    SamplesOutOfGuard = 12,  // retired
    NoPublishedMaxAge = 13,  // the hub published no resyncMaxS: fail closed
    ConfirmationStale = 14,
    NodeNotTimed      = 15,  // the node reports it is not in Mode B
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

    // The node's phase report, and the node's own decision it carries. The hub
    // used to re-judge the measurement here (clock source, sample count, mean,
    // spread, samples outside the guard). The node already applies exactly that
    // test before it arms a timed window, and it is the only end that knows the
    // result: so the hub follows the node's decision instead of a copy of it.
    (void) guard_us;

    // THE TWO STALENESS RUNGS, and the only two the optimistic switch may
    // bypass. Both mean "the hub's evidence has aged out", never "the node has
    // moved" — the rungs above (reboot, session change, missed beacon, unknown
    // firmware) are positive evidence and are tested first, so reaching here at
    // all means none of them fired.
    //
    // Rule 4 is what makes bypassing them safe: an unacked single shot is
    // retried as a burst immediately, so a wrong guess costs one frame. See
    // HubBelief::optimistic_single_shot for the arithmetic (break-even p = 6 %,
    // measured p near 1 for a node genuinely in Mode B).
    // NoPhaseReport is NOT bypassable, and the reason is worth stating because
    // the first draft of the switch did bypass it: node_timed_rx arrives IN the
    // phase report, so whenever phase_reported is false node_timed_rx is false
    // too and the bypass merely falls through to NodeNotTimed and bursts
    // anyway. It would also mean single-shotting a node the hub has never heard
    // a phase report from, which is not stale evidence — it is no evidence.
    if (!b.phase_reported)                          return TxRefusal::NoPhaseReport;
    // Nor is this one: the node reporting it is not in Mode B is the node's own
    // decision about its own windows, not an aged-out belief.
    if (!b.node_timed_rx)                           return TxRefusal::NodeNotTimed;

    if (max_age_s == 0)                             return TxRefusal::NoPublishedMaxAge;
    if (b.confirmation_age_s > max_age_s && !b.optimistic_single_shot)
                                                    return TxRefusal::ConfirmationStale;
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

// --- Keeping a kept grid honest ---------------------------------------------
//
// A demotion keeps the grid (fw 1.0.89), but a node out of Mode B hears no
// beacon, so nothing corrects its anchor while its crystal drifts. Measured
// 2026-09-15 on node 2 (fw 1.0.92): after ~17 minutes in Mode A the node
// re-promoted +8..+11 ms off its marks — inside the 14.08 ms guard by luck — and
// held +11 ms until the first beacon 95 s later. Two remedies, cheapest first.

// 1. The node re-centres its anchor on its own evidence. Samples that agree with
// one another (spread inside the guard) but sit off-centre describe a shifted
// anchor, not a noisy one, and the shift is their mean.
//
// Not in Mode B, where the beacon owns the anchor and learns the rate from it; not
// on a provisional anchor, which takes no samples; not on less evidence than
// promotion itself asks for; not for an offset small enough to promote on anyway
// (under a quarter of the guard); and not beyond half a pitch, where a sample no
// longer says which mark it belongs to. Returns the shift to add to the anchor,
// or 0 for none.
constexpr int32_t trialRecenterUs(bool in_mode_b, bool provisional, uint32_t samples,
                                  uint32_t frames, int32_t mean_us, int32_t spread_us,
                                  uint32_t guard_us, uint32_t pitch_us)
{
    if (in_mode_b || provisional)                              return 0;
    if (samples < kPromotionPhaseSamples || frames < 2)        return 0;
    if (spread_us < 0 || (uint32_t) spread_us > guard_us)      return 0;
    const int64_t mag = mean_us < 0 ? -(int64_t) mean_us : (int64_t) mean_us;
    if (mag < (int64_t) (guard_us / 4))                        return 0;
    if (mag > (int64_t) (pitch_us / 2))                        return 0;
    return mean_us;
}

// 2. The node asks the hub for its grid again (GridSyncRequest), for what its
// samples cannot fix: an anchor nothing has corrected for longer than the
// published resyncMaxS — the interval the guard band survives at the crystal
// ceiling — or samples refused as more than half a pitch off, which the re-centre
// above cannot use.
enum class SyncRequestReason : uint8_t {
    None           = 0,
    AnchorStale    = 1,
    SamplesRefused = 2,
};

static constexpr uint32_t kRefusedSamplesForSyncRequest = 2;

struct SyncRequestState
{
    bool     timed_rx_enabled   = false;
    bool     grid_active        = false;
    bool     in_mode_b          = false;
    // An encrypted uplink is possible. A plaintext request is one anybody could
    // send, and it would spend a hub burst.
    bool     session            = false;
    uint32_t resync_max_s       = 0;
    // Since the anchor was last set or corrected: GridSync, beacon, settled
    // re-solve or re-centre. 0xFFFFFFFF = never.
    uint32_t s_since_anchor_fix = 0xFFFFFFFF;
    uint32_t refused_since_fix  = 0;
    uint32_t s_since_request    = 0xFFFFFFFF;   // 0xFFFFFFFF = never asked
};

constexpr SyncRequestReason syncRequestReason(const SyncRequestState &s)
{
    if (!s.timed_rx_enabled || !s.grid_active || s.in_mode_b)  return SyncRequestReason::None;
    if (!s.session)                                            return SyncRequestReason::None;
    // Fail closed, as txRefusalFor does: no published interval, nothing to judge by.
    if (s.resync_max_s == 0)                                   return SyncRequestReason::None;
    // At most one request per resyncMaxS: a hub that does not answer is not
    // helped by being asked faster.
    if (s.s_since_request < s.resync_max_s)                    return SyncRequestReason::None;
    if (s.refused_since_fix >= kRefusedSamplesForSyncRequest)  return SyncRequestReason::SamplesRefused;
    if (s.s_since_anchor_fix > s.resync_max_s)                 return SyncRequestReason::AnchorStale;
    return SyncRequestReason::None;
}

// The hub's half. A request is advice: answered while timed mode is on and the grid
// runs, not while a GridSync is still awaiting its ack (the re-publish is already
// doing the job), and not within a minute of the last publish, so any number of
// requests costs at most one burst a minute.
static constexpr uint32_t kHubSyncRequestMinIntervalS = 60;

constexpr bool hubAnswersSyncRequest(bool timed_mode_enabled, bool grid_started,
                                     bool awaiting_ack, uint32_t s_since_publish)
{
    return timed_mode_enabled && grid_started && !awaiting_ack &&
           s_since_publish >= kHubSyncRequestMinIntervalS;
}

}  // namespace timedmode
