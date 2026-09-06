// TimedModePolicy — the asymmetry rule.
//
// The test that matters is the exhaustive negative one. Everything else is a
// convenience, because every positive case is one line of the same predicate.
//
// See docs/test-plan.md section 5.3.

#include <gtest/gtest.h>

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
    b.in_slot_acks = kPromotionUplinks;
    b.confirmation_age_s = 0;
    b.rebooted_since_confirm = false;
    b.session_changed = false;
    b.beacon_missed = false;
    b.firmware_known = true;
    b.single_shot_unacked = false;
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
    EXPECT_EQ(txPolicyFor(confident()), TxPolicy::SingleShot);
}

TEST(TimedModePolicy, AnyUncertaintyMeansBurst) {
    struct { const char *what; HubBelief b; } cases[] = {
        {"no grid",        [] { auto b = confident(); b.grid_enabled = false; return b; }()},
        {"rebooted",       [] { auto b = confident(); b.rebooted_since_confirm = true; return b; }()},
        {"session change", [] { auto b = confident(); b.session_changed = true; return b; }()},
        {"beacon missed",  [] { auto b = confident(); b.beacon_missed = true; return b; }()},
        {"firmware unknown", [] { auto b = confident(); b.firmware_known = false; return b; }()},
        {"stale",          [] { auto b = confident();
                                b.confirmation_age_s = kMaxConfirmationAgeS + 1; return b; }()},
        {"too few acks",   [] { auto b = confident(); b.in_slot_acks = 1; return b; }()},
        {"prior miss",     [] { auto b = confident(); b.single_shot_unacked = true; return b; }()},
    };
    for (auto &c : cases)
        EXPECT_EQ(txPolicyFor(c.b), TxPolicy::Burst) << c.what;
}

TEST(TimedModePolicy, ZeroInitialisedBeliefMeansBurst) {
    EXPECT_EQ(txPolicyFor(HubBelief{}), TxPolicy::Burst);
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
    for (uint32_t cage : {0u, kMaxConfirmationAgeS + 1})
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

        ++total;
        const bool bad = txPolicyFor(h) == TxPolicy::SingleShot &&
                         modeFor(n, kResyncMaxS, kGuardUs) == Mode::A;
        if (bad) {
            ++dangerous;
            // Rule 4 is the whole mitigation: after ONE unacked single shot the
            // policy must be Burst, whatever else is true.
            EXPECT_EQ(txPolicyFor(afterUnackedSingleShot(h)), TxPolicy::Burst);
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
    ASSERT_EQ(txPolicyFor(b), TxPolicy::SingleShot);
    EXPECT_EQ(txPolicyFor(afterUnackedSingleShot(b)), TxPolicy::Burst);
}
