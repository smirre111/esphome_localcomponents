// TimedModePolicy — the asymmetry rule.
//
// The test that matters is the exhaustive negative one. Everything else is a
// convenience, because every positive case is one line of the same predicate.
//
// See docs/test-plan.md section 5.3.

#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "TimedGrid.h"
#include "TimedModePolicy.h"

using namespace timedmode;

namespace {
constexpr uint32_t kResyncMaxS = 704;                 // +/-20 ppm ceiling
constexpr uint32_t kGuardUs    = timedgrid::kGuardUs; // 14080

NodeState healthy() {
    NodeState s;
    s.rtc_src = RtcSlowSrc::Crystal;
    s.grid_enabled = true;
    s.phase_valid = true;
    s.phase_err_us = 0;
    s.consecutive_missed_marks = 0;
    s.s_since_addressed_frame = 1;
    s.in_slot_uplinks = kPromotionUplinks;
    s.s_since_demotion = 0xFFFFFFFF;
    return s;
}

HubBelief confident() {
    HubBelief b;
    b.grid_enabled = true;
    b.in_slot_acks = kPromotionUplinks;   // maintained, no longer the criterion
    b.confirmation_age_s = 0;
    b.rebooted_since_confirm = false;
    b.session_changed = false;
    b.beacon_missed = false;
    b.firmware_known = true;
    b.single_shot_unacked = false;
    // The evidence single-shot is actually promoted on: the node's own phase
    // measurement, from a decrypted beacon.
    b.phase_reported  = true;
    b.phase_err_us    = 0;
    b.phase_spread_us = 0;
    b.phase_samples   = kPromotionPhaseSamples;
    b.rtc_src         = RtcSlowSrc::Crystal;
    return b;
}
}  // namespace

// ---------------------------------------------------------------------------
// The node demotes unilaterally
// ---------------------------------------------------------------------------

TEST(TimedModePolicy, HealthyNodeIsModeB) {
    EXPECT_EQ(modeFor(healthy(), kResyncMaxS, kGuardUs), Mode::B);
    EXPECT_EQ(demotionReason(healthy(), kResyncMaxS, kGuardUs), Demotion::None);
}

TEST(TimedModePolicy, RcOscillatorNodeStaysInModeAForever) {
    // ~5 % clock error cannot hold phase between beacons under any beacon
    // interval, so this is permanent and must be VISIBLE, not silent.
    for (auto src : {RtcSlowSrc::Unknown, RtcSlowSrc::InternalRc}) {
        NodeState s = healthy();
        s.rtc_src = src;
        EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::A);
        EXPECT_EQ(demotionReason(s, kResyncMaxS, kGuardUs), Demotion::BadClockSource);
    }
}

TEST(TimedModePolicy, DemotionIsImmediateOnEveryTrigger) {
    struct { const char *what; NodeState s; Demotion want; } cases[] = {
        {"grid withdrawn",  [] { auto s = healthy(); s.grid_enabled = false; return s; }(),
         Demotion::GridDisabled},
        {"missed marks",    [] { auto s = healthy();
                                 s.consecutive_missed_marks = kMaxMissedMarks; return s; }(),
         Demotion::MissedMarks},
        {"sync stale",      [] { auto s = healthy();
                                 s.s_since_addressed_frame = kResyncMaxS + 1; return s; }(),
         Demotion::SyncStale},
        {"no phase",        [] { auto s = healthy(); s.phase_valid = false; return s; }(),
         Demotion::NoPhase},
    };
    for (auto &c : cases) {
        EXPECT_EQ(demotionReason(c.s, kResyncMaxS, kGuardUs), c.want) << c.what;
        EXPECT_EQ(modeFor(c.s, kResyncMaxS, kGuardUs), Mode::A) << c.what;
    }
}

TEST(TimedModePolicy, PhaseErrorOutsideTheGuardDemotes) {
    for (int32_t e : {(int32_t) kGuardUs, -(int32_t) kGuardUs}) {
        NodeState s = healthy();
        s.phase_err_us = e;
        EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::B) << e;
    }
    for (int32_t e : {(int32_t) kGuardUs + 1, -(int32_t) kGuardUs - 1}) {
        NodeState s = healthy();
        s.phase_err_us = e;
        EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::A) << e;
    }
}

TEST(TimedModePolicy, PromotionRequiresAnUplinkObservedInItsSlot) {
    // A node claiming readiness is not evidence about where its window landed.
    for (uint32_t n = 0; n < kPromotionUplinks; ++n) {
        NodeState s = healthy();
        s.in_slot_uplinks = n;
        EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::A) << n;
        EXPECT_EQ(demotionReason(s, kResyncMaxS, kGuardUs), Demotion::NotConfirmed);
    }
}

TEST(TimedModePolicy, NoPromotionWithin10MinutesOfDemotion) {
    NodeState s = healthy();
    s.s_since_demotion = kRepromotionHoldS - 1;
    EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::A);
    EXPECT_EQ(demotionReason(s, kResyncMaxS, kGuardUs), Demotion::RecentlyDemoted);

    s.s_since_demotion = kRepromotionHoldS;
    EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::B);
}

TEST(TimedModePolicy, RefusalOrderPutsTheActionableReasonFirst) {
    // A withdrawn grid must win over everything: a node the hub has demoted
    // should not be reporting a clock complaint the operator might act on.
    NodeState s = healthy();
    s.grid_enabled = false;
    s.rtc_src = RtcSlowSrc::InternalRc;
    s.consecutive_missed_marks = 99;
    EXPECT_EQ(demotionReason(s, kResyncMaxS, kGuardUs), Demotion::GridDisabled);
}

TEST(TimedModePolicy, ZeroInitialisedStateIsModeA) {
    // proto3 defaults and a cold boot must both land in the safe mode.
    NodeState s;
    EXPECT_EQ(modeFor(s, kResyncMaxS, kGuardUs), Mode::A);
}

// ---------------------------------------------------------------------------
// The hub is conservative
// ---------------------------------------------------------------------------

TEST(TimedModePolicy, ConfidentHubMaySingleShot) {
    EXPECT_EQ(txPolicyFor(confident(), kGuardUs, kResyncMaxS), TxPolicy::SingleShot);
}

TEST(TimedModePolicy, AnyUncertaintyMeansBurst) {
    struct { const char *what; HubBelief b; } cases[] = {
        {"no grid",        [] { auto b = confident(); b.grid_enabled = false; return b; }()},
        {"rebooted",       [] { auto b = confident(); b.rebooted_since_confirm = true; return b; }()},
        {"session change", [] { auto b = confident(); b.session_changed = true; return b; }()},
        {"beacon missed",  [] { auto b = confident(); b.beacon_missed = true; return b; }()},
        {"firmware unknown", [] { auto b = confident(); b.firmware_known = false; return b; }()},
        {"stale",          [] { auto b = confident();
                                b.confirmation_age_s = kResyncMaxS + 1; return b; }()},
        {"prior miss",     [] { auto b = confident(); b.single_shot_unacked = true; return b; }()},
        // The phase report, and every way of not having a usable one.
        {"no phase report", [] { auto b = confident(); b.phase_reported = false; return b; }()},
        {"too few samples", [] { auto b = confident();
                                 b.phase_samples = kPromotionPhaseSamples - 1; return b; }()},
        {"phase late",      [] { auto b = confident();
                                 b.phase_err_us = (int32_t) kGuardUs + 1; return b; }()},
        {"phase early",     [] { auto b = confident();
                                 b.phase_err_us = -(int32_t) kGuardUs - 1; return b; }()},
        // Spread, not just the mean: two clusters one pitch apart average to
        // something innocent, and this is the case that exposes them.
        {"phase bimodal",   [] { auto b = confident(); b.phase_err_us = 0;
                                 b.phase_spread_us = (int32_t) kGuardUs + 1; return b; }()},
        // The node's own trustworthiness test, not an approximation: a tight
        // cluster offset by most of a guard band passes both the mean and the
        // spread while every frame in it lands near the edge of the window.
        {"samples outside guard", [] { auto b = confident();
                                 b.phase_outside_guard = 1; return b; }()},
        {"internal RC",     [] { auto b = confident();
                                 b.rtc_src = RtcSlowSrc::InternalRc; return b; }()},
        {"unknown clock",   [] { auto b = confident();
                                 b.rtc_src = RtcSlowSrc::Unknown; return b; }()},
    };
    for (auto &c : cases)
        EXPECT_EQ(txPolicyFor(c.b, kGuardUs, kResyncMaxS), TxPolicy::Burst) << c.what;
}

TEST(TimedModePolicy, ZeroInitialisedBeliefMeansBurst) {
    EXPECT_EQ(txPolicyFor(HubBelief{}, kGuardUs, kResyncMaxS), TxPolicy::Burst);
}

// ---------------------------------------------------------------------------
// The safety property, over the whole state space
// ---------------------------------------------------------------------------

TEST(TimedModePolicy, ExposureToADemotedNodeIsBoundedToOneFrame) {
    // Exhaustive over the cross product of everything both sides know.
    //
    // The design document claims "hub single-shot while the node is in Mode A"
    // is unreachable by construction. It is NOT, and this sweep is what shows
    // it: the hub's confirmation is a delayed observation, and rule 1 lets the
    // node demote instantly and unilaterally. No predicate over hub-local state
    // can exclude that, because the hub holds an echo of the node's state, not
    // the state.
    //
    // What IS true, and what this asserts, is bounded exposure: whenever the
    // combination occurs, one unacked frame flips the policy back to burst.
    size_t dangerous = 0, total = 0;

    for (auto src : {RtcSlowSrc::Unknown, RtcSlowSrc::InternalRc, RtcSlowSrc::Crystal})
    for (bool grid : {false, true})
    for (bool phase_valid : {false, true})
    for (int32_t err : {0, (int32_t) kGuardUs, (int32_t) kGuardUs + 1})
    for (uint32_t miss : {0u, kMaxMissedMarks - 1, kMaxMissedMarks})
    for (uint32_t age : {1u, kResyncMaxS, kResyncMaxS + 1})
    for (uint32_t upl : {0u, kPromotionUplinks})
    for (uint32_t sd : {0u, kRepromotionHoldS})
    for (bool reboot : {false, true})
    for (bool sess : {false, true})
    for (bool beacon : {false, true})
    for (bool fw : {false, true})
    for (uint32_t cage : {0u, kResyncMaxS + 1})
    // The hub's PHASE evidence, swept independently of the node's actual state.
    // That independence is the point of this test: the hub holds an echo, and
    // an echo can be stale in either direction. A report that was true when it
    // was made is exactly how the dangerous combination arises.
    for (bool h_reported : {false, true})
    for (uint32_t h_samples : {0u, kPromotionPhaseSamples})
    for (int32_t h_err : {0, (int32_t) kGuardUs + 1})
    for (uint32_t h_out : {0u, 1u})
    {
        NodeState n;
        n.rtc_src = src; n.grid_enabled = grid; n.phase_valid = phase_valid;
        n.phase_err_us = err; n.consecutive_missed_marks = miss;
        n.s_since_addressed_frame = age; n.in_slot_uplinks = upl;
        n.s_since_demotion = sd;

        HubBelief h;
        h.grid_enabled = grid; h.in_slot_acks = upl; h.confirmation_age_s = cage;
        h.rebooted_since_confirm = reboot; h.session_changed = sess;
        h.beacon_missed = beacon; h.firmware_known = fw;
        h.single_shot_unacked = false;
        h.phase_reported = h_reported; h.phase_samples = h_samples;
        h.phase_err_us = h_err; h.phase_spread_us = 0;
        h.phase_outside_guard = h_out;
        // The hub learns the clock source from the same beacon, so this one
        // tracks the node rather than being swept separately.
        h.rtc_src = src;

        ++total;
        const bool bad = txPolicyFor(h, kGuardUs, kResyncMaxS) == TxPolicy::SingleShot &&
                         modeFor(n, kResyncMaxS, kGuardUs) == Mode::A;
        if (bad) {
            ++dangerous;
            // Rule 4 is the whole mitigation: after ONE unacked single shot the
            // policy must be Burst, whatever else is true.
            EXPECT_EQ(txPolicyFor(afterUnackedSingleShot(h), kGuardUs, kResyncMaxS), TxPolicy::Burst);
        }
    }

    EXPECT_GT(total, 1000u);
    // Documented, not tolerated: the combination exists and is bounded.
    EXPECT_GT(dangerous, 0u)
        << "if this ever becomes 0, the stronger 'unreachable by construction' "
           "claim has become true and the comment above should be corrected";
}

TEST(TimedModePolicy, UnackedSingleShotAlwaysForcesBurst) {
    HubBelief b = confident();
    ASSERT_EQ(txPolicyFor(b, kGuardUs, kResyncMaxS), TxPolicy::SingleShot);
    EXPECT_EQ(txPolicyFor(afterUnackedSingleShot(b), kGuardUs, kResyncMaxS), TxPolicy::Burst);
}

// ---------------------------------------------------------------------------
// The staleness bound is the interval the hub PUBLISHED
// ---------------------------------------------------------------------------

TEST(TimedModePolicy, StalenessIsJudgedAgainstThePublishedResyncInterval) {
    // The node demotes itself after resyncMaxS without an addressed frame, so a
    // hub that trusted a report older than that would be sending single copies
    // to a node already back on a free-running window. Both ends use the same
    // number, and the hub uses the one it actually sent.
    HubBelief b = confident();
    b.confirmation_age_s = kResyncMaxS;
    EXPECT_EQ(txPolicyFor(b, kGuardUs, kResyncMaxS), TxPolicy::SingleShot);

    b.confirmation_age_s = kResyncMaxS + 1;
    EXPECT_EQ(txPolicyFor(b, kGuardUs, kResyncMaxS), TxPolicy::Burst);

    // A shorter published interval binds harder — the hub is not free to keep
    // trusting a report past what it told the node.
    b.confirmation_age_s = 400;
    EXPECT_EQ(txPolicyFor(b, kGuardUs, kResyncMaxS), TxPolicy::SingleShot);
    EXPECT_EQ(txPolicyFor(b, kGuardUs, 350), TxPolicy::Burst);
}

TEST(TimedModePolicy, NoPublishedIntervalMeansNoPromotion) {
    // Zero is what a hub that has published no grid holds. Refusing is the same
    // direction as every other default here: the unset value is the safe one,
    // and "unbounded trust" would be the one reading that is never right.
    HubBelief b = confident();
    b.confirmation_age_s = 0;
    EXPECT_EQ(txPolicyFor(b, kGuardUs, 0), TxPolicy::Burst);
}

TEST(TimedModePolicy, A60SecondBoundWouldHaveBeenInert) {
    // Recorded because it shipped and did nothing. The phase report rides
    // uplinks the node already sends, and at 3.5 commands/node/day the newest
    // one is minutes to hours old — so a 60 s ceiling refused every promotion
    // while looking like a working rule. The bound has to be the interval over
    // which the measurement stays valid, not a number that feels cautious.
    HubBelief b = confident();
    b.confirmation_age_s = 300;                       // a very recent uplink
    EXPECT_EQ(txPolicyFor(b, kGuardUs, 60), TxPolicy::Burst);
    EXPECT_EQ(txPolicyFor(b, kGuardUs, kResyncMaxS), TxPolicy::SingleShot);
}

// ---------------------------------------------------------------------------
// U-1: the REASON, not just the answer
//
// txPolicyFor answered Burst-or-SingleShot and nothing else, so a node stuck on
// bursts — paying seventeen copies for every frame, which is the entire cost
// Mode B exists to remove — gave an operator nothing to act on. The boolean was
// never the diagnostic; which of fourteen conditions failed is.
// ---------------------------------------------------------------------------

TEST(TimedModePolicy, EveryRefusalReasonIsReachableAndNamesItsOwnCondition) {
    // One case per rung. If a rung is ever added without a case here, the
    // exhaustiveness check at the bottom of this test fails.
    struct Case { const char *what; TxRefusal want; HubBelief b; };
    std::vector<Case> cases;

    auto with = [](void (*mutate)(HubBelief &)) {
        HubBelief b = confident();
        mutate(b);
        return b;
    };

    cases.push_back({"no grid", TxRefusal::GridDisabled,
                     with([](HubBelief &b) { b.grid_enabled = false; })});
    cases.push_back({"rebooted", TxRefusal::RebootedSinceConfirm,
                     with([](HubBelief &b) { b.rebooted_since_confirm = true; })});
    cases.push_back({"new session", TxRefusal::SessionChanged,
                     with([](HubBelief &b) { b.session_changed = true; })});
    cases.push_back({"beacon missed", TxRefusal::BeaconMissed,
                     with([](HubBelief &b) { b.beacon_missed = true; })});
    cases.push_back({"firmware unknown", TxRefusal::FirmwareUnknown,
                     with([](HubBelief &b) { b.firmware_known = false; })});
    cases.push_back({"rule 4", TxRefusal::SingleShotUnacked,
                     with([](HubBelief &b) { b.single_shot_unacked = true; })});
    cases.push_back({"no phase report", TxRefusal::NoPhaseReport,
                     with([](HubBelief &b) { b.phase_reported = false; })});
    cases.push_back({"internal RC", TxRefusal::BadClockSource,
                     with([](HubBelief &b) { b.rtc_src = RtcSlowSrc::InternalRc; })});
    cases.push_back({"too few samples", TxRefusal::TooFewSamples,
                     with([](HubBelief &b) { b.phase_samples = kPromotionPhaseSamples - 1; })});
    cases.push_back({"mean out of guard", TxRefusal::PhaseOutOfGuard,
                     with([](HubBelief &b) { b.phase_err_us = (int32_t) kGuardUs + 1; })});
    cases.push_back({"bimodal", TxRefusal::SpreadTooWide,
                     with([](HubBelief &b) { b.phase_spread_us = (int32_t) kGuardUs + 1; })});
    cases.push_back({"samples outside", TxRefusal::SamplesOutOfGuard,
                     with([](HubBelief &b) { b.phase_outside_guard = 1; })});
    cases.push_back({"stale confirmation", TxRefusal::ConfirmationStale,
                     with([](HubBelief &b) { b.confirmation_age_s = kResyncMaxS + 1; })});

    for (const auto &c : cases) {
        EXPECT_EQ(txRefusalFor(c.b, kGuardUs, kResyncMaxS), c.want) << c.what;
        EXPECT_EQ(txPolicyFor(c.b, kGuardUs, kResyncMaxS), TxPolicy::Burst) << c.what;
    }

    // NoPublishedMaxAge is the one rung that is not a belief field — it is the
    // hub having published no resyncMaxS at all, which must fail closed.
    EXPECT_EQ(txRefusalFor(confident(), kGuardUs, 0), TxRefusal::NoPublishedMaxAge);

    // Exhaustiveness: every enumerator except None must be produced by one of
    // the cases above. A rung added to the ladder without a case here is a
    // reason an operator would see as a number with nothing behind it.
    std::set<TxRefusal> produced;
    for (const auto &c : cases) produced.insert(c.want);
    produced.insert(TxRefusal::NoPublishedMaxAge);
    for (uint8_t v = 1; v <= (uint8_t) TxRefusal::ConfirmationStale; ++v) {
        EXPECT_EQ(produced.count((TxRefusal) v), 1u)
            << "TxRefusal value " << (int) v << " is never produced by any case "
               "in this test — either the ladder gained a rung without a case, "
               "or the enum gained a value the ladder cannot return";
    }
}

TEST(TimedModePolicy, TheAnswerAndTheReasonCannotDisagree) {
    // txPolicyFor is DERIVED from txRefusalFor rather than repeating the
    // ladder, which is the point: two copies would drift, and the drift would
    // be silent — the hub bursting while reporting a reason that says it should
    // not, or promoting while reporting one that says it should not.
    //
    // Swept rather than spot-checked, over every single-field deviation from a
    // confident belief plus the confident belief itself.
    std::vector<HubBelief> all;
    all.push_back(confident());
    auto push = [&all](void (*mutate)(HubBelief &)) {
        HubBelief b = confident();
        mutate(b);
        all.push_back(b);
    };
    push([](HubBelief &b) { b.grid_enabled = false; });
    push([](HubBelief &b) { b.rebooted_since_confirm = true; });
    push([](HubBelief &b) { b.session_changed = true; });
    push([](HubBelief &b) { b.beacon_missed = true; });
    push([](HubBelief &b) { b.firmware_known = false; });
    push([](HubBelief &b) { b.single_shot_unacked = true; });
    push([](HubBelief &b) { b.phase_reported = false; });
    push([](HubBelief &b) { b.rtc_src = RtcSlowSrc::Unknown; });
    push([](HubBelief &b) { b.phase_samples = 0; });
    push([](HubBelief &b) { b.phase_err_us = -((int32_t) kGuardUs) - 1; });
    push([](HubBelief &b) { b.phase_spread_us = (int32_t) kGuardUs + 1; });
    push([](HubBelief &b) { b.phase_outside_guard = 3; });
    push([](HubBelief &b) { b.confirmation_age_s = kResyncMaxS + 1; });

    for (uint32_t max_age : {0u, 60u, (uint32_t) kResyncMaxS}) {
        for (const auto &b : all) {
            const bool single = txPolicyFor(b, kGuardUs, max_age) == TxPolicy::SingleShot;
            const bool none   = txRefusalFor(b, kGuardUs, max_age) == TxRefusal::None;
            EXPECT_EQ(single, none)
                << "the published reason and the actual decision disagree at "
                   "max_age " << max_age;
        }
    }
}
