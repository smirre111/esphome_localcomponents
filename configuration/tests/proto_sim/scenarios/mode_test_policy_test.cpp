// ModeTest's decisions — the on-hardware validation mode, minus the hardware.
//
// Everything here is a rule that must hold whether or not a radio is attached.
// Two of them exist because of a specific failure that has already happened
// once, and those get the hardest tests: the phase-lock that made ~300 grid
// frames produce zero receptions, and the unauthenticated arm that would let
// the bootstrap window park a fleet in a test mode.
//
// See docs/test-plan.md section 10.

#include <gtest/gtest.h>

#include "ModeTestPolicy.h"

using namespace modetest;

namespace {
NodeContext armable() {
    NodeContext c;
    c.frame_authenticated = true;
    c.has_session    = true;
    c.is_bench_node  = false;
    // A node that has everything it needs to arm, which for MODE_B and
    // MODE_SWEEP includes an adopted grid — there is no window to arm without
    // one. The tests that are ABOUT that requirement clear it explicitly.
    c.has_adopted_grid = true;
    c.battery_mv     = 4000;
    c.rx_interval_ms = 500;
    return c;
}
Request modeA(uint32_t period_ms = 1100) {
    Request r;
    r.mode           = Mode::A;
    r.grid_period_ms = period_ms;
    r.copies         = 1;
    return r;
}
}  // namespace

// ---------------------------------------------------------------------------
// Duration — the node owns it
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, DurationIsCappedByTheNodeNotTheHub) {
    EXPECT_EQ(testDurationS(0), kDefaultDurationS) << "0 means the node's default";
    EXPECT_EQ(testDurationS(60), 60u);
    EXPECT_EQ(testDurationS(kMaxDurationS), kMaxDurationS);
    EXPECT_EQ(testDurationS(kMaxDurationS + 1), kMaxDurationS);
    EXPECT_EQ(testDurationS(0xFFFFFFFF), kMaxDurationS)
        << "a hub that asks for forever gets fifteen minutes";
}

TEST(ModeTestPolicy, TheCeilingIsTighterThanDriftTests) {
    // DriftTest caps at 1800 s. ModeTest can hold continuous RX AND force a
    // mode, so an abandoned run costs more.
    EXPECT_LT(kMaxDurationS, 1800u);
}

// ---------------------------------------------------------------------------
// Commensurate periods — the failure that has already happened
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, ACommensurateGridPeriodIsRefusedInModeA) {
    // 1000 ms against 500 ms windows phase-locks: a frame that lands in an
    // RX-off gap does so forever. Observed on DriftTest — ~300 frames sent,
    // zero heard. This is the whole reason DriftTest runs at 1100 ms.
    EXPECT_EQ(armRefusal(modeA(1000), armable()), ArmRefusal::CommensurateGrid);
    EXPECT_EQ(armRefusal(modeA(500),  armable()), ArmRefusal::CommensurateGrid);
    EXPECT_EQ(armRefusal(modeA(1500), armable()), ArmRefusal::CommensurateGrid);
    EXPECT_EQ(armRefusal(modeA(250),  armable()), ArmRefusal::CommensurateGrid)
        << "the divisor direction counts too";

    EXPECT_EQ(armRefusal(modeA(1100), armable()), ArmRefusal::None)
        << "DriftTest's period, chosen for exactly this reason";
}

TEST(ModeTestPolicy, ModeBRequiresTheCommensuratePeriodItWouldOtherwiseRefuse) {
    // In Mode B the rule inverts: the grid period must BE the round and
    // phase-locked to the node's slot. That is the mode, not a mistake.
    Request b;
    b.mode           = Mode::B;
    b.grid_period_ms = 1500;          // exactly commensurate with 500
    b.copies         = 1;
    EXPECT_EQ(armRefusal(b, armable()), ArmRefusal::None);

    EXPECT_FALSE(periodMattersFor(Mode::B));
    EXPECT_FALSE(periodMattersFor(Mode::C));
    EXPECT_TRUE(periodMattersFor(Mode::A));
    EXPECT_TRUE(periodMattersFor(Mode::Sweep));
}

TEST(ModeTestPolicy, CommensurabilityIsSymmetricAndZeroSafe) {
    EXPECT_TRUE(periodsAreCommensurate(1000, 500));
    EXPECT_TRUE(periodsAreCommensurate(500, 1000));
    EXPECT_TRUE(periodsAreCommensurate(500, 500));
    EXPECT_FALSE(periodsAreCommensurate(1100, 500));
    EXPECT_FALSE(periodsAreCommensurate(0, 500)) << "no period is not a phase lock";
    EXPECT_FALSE(periodsAreCommensurate(1000, 0));
}

TEST(ModeTestPolicy, AModeThatNeedsARulerIsRefusedWithoutOne) {
    Request r = modeA(0);
    EXPECT_EQ(armRefusal(r, armable()), ArmRefusal::NoGridPeriod);
}

// ---------------------------------------------------------------------------
// Arming is authenticated even though the traffic is not
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, ANodeWithNoSessionRefusesToArm) {
    // The frames a ModeTest produces need no session — MAC-0 is the state every
    // node passes through on every cold boot. Arming one is different: without
    // this rule the unauthenticated bootstrap window is a way to park 32 nodes
    // in a test mode.
    NodeContext c = armable();
    c.has_session = false;
    EXPECT_EQ(armRefusal(modeA(), c), ArmRefusal::NoSession);
}

TEST(ModeTestPolicy, TheSessionCheckComesFirst) {
    // An unauthenticated request that is ALSO malformed must still be refused
    // for the reason that matters. If a friendlier refusal returned first, a
    // caller could probe which of its fields the node dislikes without ever
    // holding a session.
    NodeContext c = armable();
    c.has_session = false;
    c.battery_mv  = 3000;              // also too low
    Request r = modeA(1000);           // also commensurate
    r.copies  = 99;                    // also out of range
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::NoSession);
}

TEST(ModeTestPolicy, SweepIsRefusedOffTheBench) {
    // MODE_SWEEP deliberately mis-arms windows to find the reception edge. On a
    // deployed node that is just a node that stops hearing the hub.
    Request r;
    r.mode           = Mode::Sweep;
    r.grid_period_ms = 1100;
    r.copies         = 1;

    EXPECT_EQ(armRefusal(r, armable()), ArmRefusal::SweepOffBench);

    NodeContext bench = armable();
    bench.is_bench_node = true;
    EXPECT_EQ(armRefusal(r, bench), ArmRefusal::None);
}

TEST(ModeTestPolicy, AFlatBatteryRefusesTheTest) {
    NodeContext c = armable();
    c.battery_mv = kMinBatteryMv - 1;
    EXPECT_EQ(armRefusal(modeA(), c), ArmRefusal::BatteryTooLow);

    c.battery_mv = kMinBatteryMv;
    EXPECT_EQ(armRefusal(modeA(), c), ArmRefusal::None) << "the threshold is inclusive";
}

TEST(ModeTestPolicy, CopiesOutsideAFleetBurstAreRefused) {
    Request r = modeA();
    r.copies = 0;
    EXPECT_EQ(armRefusal(r, armable()), ArmRefusal::BadCopies);
    r.copies = 18;
    EXPECT_EQ(armRefusal(r, armable()), ArmRefusal::BadCopies)
        << "the hub never sends more than txSlotsPerRound, so 18 describes "
           "nothing this fleet does";
    for (uint32_t c = kMinCopies; c <= kMaxCopies; ++c) {
        r.copies = c;
        EXPECT_EQ(armRefusal(r, armable()), ArmRefusal::None) << "copies " << c;
    }
}

// ---------------------------------------------------------------------------
// Distributions, not means
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, AnEmptySampleSetSummarisesToZerosNotGarbage) {
    Samples s;
    const Hist h = s.summarise();
    EXPECT_EQ(h.n, 0u);
    EXPECT_EQ(h.min, 0);
    EXPECT_EQ(h.max, 0);
    EXPECT_EQ(h.p99, 0);
}

TEST(ModeTestPolicy, OrderStatisticsAreTheObservedValuesNotInterpolations) {
    Samples s;
    for (int i = 1; i <= 100; ++i) s.add(i);

    const Hist h = s.summarise();
    EXPECT_EQ(h.n, 100u);
    EXPECT_EQ(h.min, 1);
    EXPECT_EQ(h.max, 100);
    EXPECT_EQ(h.p50, 50);
    EXPECT_EQ(h.p95, 95);
    EXPECT_EQ(h.p99, 99);
}

TEST(ModeTestPolicy, InsertionOrderDoesNotChangeTheAnswer) {
    Samples asc, desc;
    for (int i = 1; i <= 50; ++i) asc.add(i);
    for (int i = 50; i >= 1; --i) desc.add(i);

    const Hist a = asc.summarise();
    const Hist d = desc.summarise();
    EXPECT_EQ(a.min, d.min);
    EXPECT_EQ(a.p50, d.p50);
    EXPECT_EQ(a.p95, d.p95);
    EXPECT_EQ(a.p99, d.p99);
    EXPECT_EQ(a.max, d.max);
}

TEST(ModeTestPolicy, ATailOutlierSurvivesSummarising) {
    // The point of reporting p99 and max rather than a mean. Section 12.3
    // expects the RxDone latency to be multi-modal, because production scales
    // 40-240 MHz and the motor's PM lock moves the frequency mid-operation.
    Samples s;
    for (int i = 0; i < 99; ++i) s.add(100);
    s.add(9000);

    const Hist h = s.summarise();
    EXPECT_EQ(h.p50, 100);
    EXPECT_EQ(h.max, 9000) << "a mean would have reported 189 and hidden it";
}

TEST(ModeTestPolicy, NegativeSamplesAreOrderedCorrectly) {
    // phaseErrUs is signed: a window armed early is as real as one armed late,
    // and an unsigned buffer would have folded the early half onto the late.
    Samples s;
    for (int i = -50; i <= 50; ++i) s.add(i);
    const Hist h = s.summarise();
    EXPECT_EQ(h.min, -50);
    EXPECT_EQ(h.max, 50);
    EXPECT_EQ(h.p50, 0);
}

TEST(ModeTestPolicy, TheBufferKeepsSpanningTheRunOnceItIsFull) {
    // A fixed buffer that simply stopped accepting samples would summarise the
    // first two minutes of a fifteen-minute run and call it the run.
    Samples s;
    for (uint32_t i = 0; i < kMaxSamples * 4; ++i) s.add((int32_t) i);

    const Hist h = s.summarise();
    EXPECT_EQ(h.n, kMaxSamples * 4) << "n reports what was OFFERED, not what was kept";
    EXPECT_GT(h.max, (int32_t) (kMaxSamples * 3))
        << "late samples must still be able to reach the summary";
    EXPECT_EQ(h.min, 0) << "and the earliest extreme is not thrown away";
}

TEST(ModeTestPolicy, PercentileIndexIsClampedAndNearestRank) {
    EXPECT_EQ(Samples::percentileIndex(0, 99), 0u);
    EXPECT_EQ(Samples::percentileIndex(1, 99), 0u);
    EXPECT_EQ(Samples::percentileIndex(10, 50), 4u);
    EXPECT_EQ(Samples::percentileIndex(10, 99), 9u)
        << "with ten samples the p99 slot IS the maximum, and saying so is "
           "honest where an interpolation would invent a value";
    EXPECT_EQ(Samples::percentileIndex(100, 100), 99u);
}

// ---------------------------------------------------------------------------
// Sequence gaps — independent of MAC-1
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, AGapIsCountedWithoutTheFrameCounterBeingOn) {
    // seq is the hub's mark index, deliberately not msgid: msgid is also the
    // AEAD nonce input and the replay-filter key, so the test could not
    // retransmit without disturbing both.
    SeqTracker t;
    t.note(10); t.note(11); t.note(14); t.note(15);

    EXPECT_EQ(t.first, 10u);
    EXPECT_EQ(t.last, 15u);
    EXPECT_EQ(t.received, 4u);
    EXPECT_EQ(t.gaps, 2u) << "12 and 13";
    EXPECT_EQ(t.expected(), 6u);
    EXPECT_EQ(t.expected() - t.received, t.gaps);
}

TEST(ModeTestPolicy, ARetransmitIsNotAGap) {
    SeqTracker t;
    t.note(5); t.note(6); t.note(6); t.note(7);
    EXPECT_EQ(t.gaps, 0u);
    EXPECT_EQ(t.last, 7u);
    EXPECT_EQ(t.received, 4u) << "a duplicate still arrived";
}

TEST(ModeTestPolicy, AReorderDoesNotRewindTheHighWaterMark) {
    SeqTracker t;
    t.note(5); t.note(9); t.note(6);
    EXPECT_EQ(t.last, 9u) << "9 was still the furthest the hub got";
    EXPECT_EQ(t.gaps, 3u) << "6, 7, 8 were missing when 9 arrived";
}

TEST(ModeTestPolicy, NothingReceivedIsNotAThousandGaps) {
    SeqTracker t;
    EXPECT_EQ(t.expected(), 0u);
    EXPECT_EQ(t.gaps, 0u);
    EXPECT_FALSE(t.started);
}

// ---------------------------------------------------------------------------
// Restoring
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, AnUncapturedStateRestoresNothing) {
    // Every exit path calls restore, including the timer's, and one of them can
    // run before anything was saved. `valid` is what makes that a no-op instead
    // of a write of zeroes over the node's real mode and slot.
    SavedState s;
    EXPECT_FALSE(s.valid);
}

TEST(ModeTestPolicy, TheDefaultsAreTheProductionConfiguration) {
    // A zeroed or partially-filled SavedState must restore to the SAFE state,
    // not the permissive one — the same reason MacSublayers::Config defaults
    // both sublayers on.
    SavedState s;
    EXPECT_TRUE(s.power_profile_production);
    EXPECT_FALSE(s.continuous_rx);
    EXPECT_TRUE(s.counter_enabled);
    EXPECT_TRUE(s.crypto_enabled);
    EXPECT_TRUE(s.deep_sleep_allowed);
}

// ---------------------------------------------------------------------------
// The arming frame must itself have been authenticated
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, APlaintextArmIsRefusedEvenWithALiveSession) {
    // mac-layer.md §4: "a plaintext frame asking to turn authentication off
    // answers its own question." MacSublayers::armRefusal has always taken this
    // parameter; ModeTest's did not, and handleModeTest then wrote the SAME two
    // sublayer variables applyMacConfig_ guards — counter_enabled and
    // crypto_enabled — from proto3 fields defaulting to false. A plaintext
    // ModeTest against a node with a live session therefore disabled MAC-1 and
    // MAC-2, and pinned the CPU at 240 MHz with light sleep off because the
    // power profile was carried as `keepPowerProfile`, a proto3 bool defaulting
    // to false despite its comment saying "DEFAULT TRUE". That field is now
    // `dropPowerProfile`, so proto3's zero is the safe answer — but the
    // authentication gate is what this test is for, and it is unchanged.
    NodeContext c = armable();
    c.frame_authenticated = false;
    EXPECT_EQ(armRefusal(modeA(), c), ArmRefusal::NotAuthenticated);
}

TEST(ModeTestPolicy, AuthenticationIsCheckedBeforeAnythingElse) {
    // Same reasoning as the session check: a caller must not be able to probe
    // which of its fields the node dislikes without holding a key.
    NodeContext c = armable();
    c.frame_authenticated = false;
    c.has_session         = false;
    c.battery_mv          = 3000;
    Request r = modeA(1000);          // also commensurate
    r.copies  = 99;                   // also out of range
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::NotAuthenticated);
}

TEST(ModeTestPolicy, TheDefaultContextRefusesRatherThanArms) {
    // A NodeContext that forgets to set the flag must refuse. Same direction as
    // every other default in these headers: the zeroed value is the safe one.
    NodeContext fresh;
    EXPECT_FALSE(fresh.frame_authenticated);
    EXPECT_EQ(armRefusal(modeA(), fresh), ArmRefusal::NotAuthenticated);
}

// ---------------------------------------------------------------------------
// Which mode the test actually runs
//
// The defect these pin: `mode` travelled the wire, was stored, was echoed into
// the report, and never changed the node's RX discipline. A "Mode Test B" run
// measured Mode A and the hub logged it as a Mode B result — so every Mode B
// number the fleet could produce described the wrong mode.
// ---------------------------------------------------------------------------

TEST(ModeTestPolicy, ModeBWithoutAnAdoptedGridIsRefused) {
    // Timed windows are armed against a grid. With no anchor there is nothing
    // to arm against, and enabling timed RX anyway does not measure Mode B
    // badly — it stops the node hearing the hub for the length of the test.
    NodeContext c = armable();
    c.has_adopted_grid = false;
    Request r;
    r.mode   = Mode::B;
    r.copies = 1;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::NoGrid);

    c.has_adopted_grid = true;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::None)
        << "MODE_B needs no grid PERIOD — the round is the period";
}

TEST(ModeTestPolicy, SweepNeedsBothTheBenchAndAGrid) {
    NodeContext c = armable();
    c.is_bench_node    = true;
    c.has_adopted_grid = false;
    Request r;
    r.mode           = Mode::Sweep;
    r.grid_period_ms = 1100;
    r.copies         = 1;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::NoGrid);

    // And the bench check still comes first: a field node is refused for being
    // a field node, whatever its grid state.
    c.is_bench_node    = false;
    c.has_adopted_grid = true;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::SweepOffBench);
}

TEST(ModeTestPolicy, ModeCIsRefusedRatherThanSilentlyNotApplied) {
    // Class A is a sleep discipline, not a flag: the node wakes, transmits,
    // opens RX1/RX2 and sleeps. Refusing is what stops a report carrying
    // `mode = 3` over a run that never left Mode A.
    NodeContext c = armable();
    c.has_adopted_grid = true;
    Request r;
    r.mode   = Mode::C;
    r.copies = 1;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::ModeUnimplemented);
    EXPECT_FALSE(modeIsImplemented(Mode::C));
    EXPECT_TRUE(modeIsImplemented(Mode::A));
    EXPECT_TRUE(modeIsImplemented(Mode::B));
    EXPECT_TRUE(modeIsImplemented(Mode::Sweep));
}

TEST(ModeTestPolicy, ACapabilityRefusalOutranksTheRequestsParameters) {
    // A node refused for MODE_C or for having no grid must say so, not send an
    // operator off to charge a battery or pick a different period.
    NodeContext c = armable();
    c.has_adopted_grid = false;
    c.battery_mv       = 3000;            // also too low
    Request r;
    r.mode   = Mode::B;
    r.copies = 99;                        // also out of range
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::NoGrid);

    r.mode = Mode::C;
    EXPECT_EQ(armRefusal(r, c), ArmRefusal::ModeUnimplemented);
}

TEST(ModeTestPolicy, TheReportsModeComesFromStateNotFromTheRequest) {
    // The whole defect in one assertion: asking for B and not applying it must
    // report A.
    EXPECT_EQ(modeApplied(Mode::B, false), Mode::A);
    EXPECT_EQ(modeApplied(Mode::B, true),  Mode::B);
    EXPECT_EQ(modeApplied(Mode::A, false), Mode::A);

    // UNSPEC leaves the mode alone, so it reports whatever the node was in.
    EXPECT_EQ(modeApplied(Mode::Unspec, true),  Mode::B);
    EXPECT_EQ(modeApplied(Mode::Unspec, false), Mode::A);

    // Sweep is a timed-window run distinguished by its arm offset, so it keeps
    // its own identity rather than collapsing into B.
    EXPECT_EQ(modeApplied(Mode::Sweep, true), Mode::Sweep);
}

TEST(ModeTestPolicy, OnlyTimedModesNeedAGrid) {
    EXPECT_TRUE(modeNeedsGrid(Mode::B));
    EXPECT_TRUE(modeNeedsGrid(Mode::Sweep));
    EXPECT_FALSE(modeNeedsGrid(Mode::A));
    EXPECT_FALSE(modeNeedsGrid(Mode::Unspec));
}
