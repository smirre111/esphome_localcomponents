// MotorPolicy — the decisions the motor FSM is made of.
//
// This is the part of the node that could previously only be tested by
// watching a blind move. fsmProcess() was CCN 65 and fsmNextState() CCN 30,
// both reachable only with a motor attached, and between them they decide
// where the blind is and when to stop — by four mechanisms that must not fight
// each other.
//
// Every failure in here presents identically: a blind that stops in the wrong
// place. The only instrument in the field is a stopwatch, which is why these
// invariants are pinned here instead.

#include <gtest/gtest.h>

#include <cmath>

#include "MotorPolicy.h"

using namespace motorpolicy;

namespace {

// A self-consistent roll geometry: a blind of kHeight wound at kThickness onto
// a kAxleR axle fills exactly to kFullR, so position spans 0..1 across the
// radius range.
//
// Getting this consistent matters — the first version of this fixture used a
// full radius that could hold ~5 m of blind for a 1.5 m cover, so position
// saturated at 1.0 less than halfway through the travel and the non-linearity
// test failed against a clamp rather than against the physics.
//
//   s(R) = (pi / t) * (R^2 - r_axle^2) = height
//   => R = sqrt(height * t / pi + r_axle^2)
//        = sqrt(1500 * 0.5 / pi + 100) ~= 18.41 mm
constexpr float    kAxleR     = 10.0f;    // mm
constexpr float    kFullR     = 18.41f;   // mm
constexpr float    kThickness = 0.5f;     // mm
constexpr float    kHeight    = 1500.0f;  // mm
constexpr uint32_t kOpenMs    = 38000;
constexpr uint32_t kCloseMs   = 38000;

}  // namespace

// ---------------------------------------------------------------------------
// The command FSM
// ---------------------------------------------------------------------------

TEST(MotorPolicy, IdleStartsEachKindOfMove) {
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_FULL_UP).next,   BLINDS_FULLY_OPENING);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_FULL_DOWN).next, BLINDS_FULLY_CLOSING);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_STEP_UP).next,   BLINDS_STEP_UP);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_STEP_DOWN).next, BLINDS_STEP_DOWN);
}

TEST(MotorPolicy, NothingIsStoppedFirstFromIdle) {
    // There is nothing to stop, and issuing a stop before starting would be a
    // wasted relay cycle on a battery node.
    EXPECT_FALSE(nextState(BLINDS_IDLE, MOTCMD_FULL_UP).stopFirst);
    EXPECT_FALSE(nextState(BLINDS_IDLE, MOTCMD_FULL_DOWN).stopFirst);
}

TEST(MotorPolicy, ReversingAlwaysStopsFirst) {
    // Driving straight from one direction into the other would slam the motor.
    //
    // Every reversing pair, not a sample of them: an earlier version of this
    // test checked four of the eight, and a mutation that dropped stopFirst
    // from STEP_UP + FULL_DOWN went unnoticed.
    struct { BlindsState_t from; MotorCmd_t cmd; } reversals[] = {
        {BLINDS_STEP_UP,       MOTCMD_STEP_DOWN}, {BLINDS_STEP_UP,       MOTCMD_FULL_DOWN},
        {BLINDS_FULLY_OPENING, MOTCMD_STEP_DOWN}, {BLINDS_FULLY_OPENING, MOTCMD_FULL_DOWN},
        {BLINDS_STEP_DOWN,     MOTCMD_STEP_UP},   {BLINDS_STEP_DOWN,     MOTCMD_FULL_UP},
        {BLINDS_FULLY_CLOSING, MOTCMD_STEP_UP},   {BLINDS_FULLY_CLOSING, MOTCMD_FULL_UP},
    };
    for (const auto &r : reversals) {
        EXPECT_TRUE(nextState(r.from, r.cmd).stopFirst)
            << "state " << (int) r.from << " + cmd " << (int) r.cmd;
    }
}

TEST(MotorPolicy, ContinuingTheSameDirectionDoesNotStopFirst) {
    EXPECT_FALSE(nextState(BLINDS_STEP_UP,   MOTCMD_FULL_UP).stopFirst);
    EXPECT_FALSE(nextState(BLINDS_STEP_DOWN, MOTCMD_FULL_DOWN).stopFirst);
}

TEST(MotorPolicy, AFullMoveIgnoresFurtherCommandsInTheSameDirection) {
    // The asymmetry that is easiest to "tidy" into a bug. Restarting the fade
    // would restart the travel clock, and the position estimate is integrated
    // from that clock — the blind would believe it is further from the end than
    // it is, and stop short.
    EXPECT_EQ(nextState(BLINDS_FULLY_OPENING, MOTCMD_FULL_UP).next,   BLINDS_FULLY_OPENING);
    EXPECT_EQ(nextState(BLINDS_FULLY_OPENING, MOTCMD_FULL_UP).drive,  Drive::None);
    EXPECT_EQ(nextState(BLINDS_FULLY_OPENING, MOTCMD_STEP_UP).drive,  Drive::None);

    EXPECT_EQ(nextState(BLINDS_FULLY_CLOSING, MOTCMD_FULL_DOWN).next,  BLINDS_FULLY_CLOSING);
    EXPECT_EQ(nextState(BLINDS_FULLY_CLOSING, MOTCMD_FULL_DOWN).drive, Drive::None);
    EXPECT_EQ(nextState(BLINDS_FULLY_CLOSING, MOTCMD_STEP_DOWN).drive, Drive::None);
}

TEST(MotorPolicy, StopAlwaysReachesIdleFromAnyMovingState) {
    for (BlindsState_t s : {BLINDS_STEP_UP, BLINDS_STEP_DOWN,
                            BLINDS_FULLY_OPENING, BLINDS_FULLY_CLOSING}) {
        const Transition t = nextState(s, MOTCMD_STOP);
        EXPECT_EQ(t.next,  BLINDS_IDLE) << "state " << (int) s;
        EXPECT_EQ(t.drive, Drive::Stop) << "state " << (int) s;
        EXPECT_FALSE(t.snap) << "a manual stop must leave the position where the "
                                "blind actually is, not snap it to an extreme";
    }
}

TEST(MotorPolicy, ATimerEndingAFullMoveSnapsToTheExtreme) {
    // The move ran its whole configured duration, so the blind is against the
    // stop whatever the integrated estimate drifted to.
    const Transition up = nextState(BLINDS_FULLY_OPENING, MOTCMD_TIMER);
    EXPECT_EQ(up.next, BLINDS_IDLE);
    ASSERT_TRUE(up.snap);
    EXPECT_FLOAT_EQ(up.snapTo, 1.0f);

    const Transition down = nextState(BLINDS_FULLY_CLOSING, MOTCMD_TIMER);
    EXPECT_EQ(down.next, BLINDS_IDLE);
    ASSERT_TRUE(down.snap);
    EXPECT_FLOAT_EQ(down.snapTo, 0.0f);
}

TEST(MotorPolicy, ATimerEndingAStepDoesNotSnap) {
    // The other half of that asymmetry. A step is a nudge that ends wherever it
    // ends; snapping it would teleport the estimate to an end the blind never
    // reached, and every later move would start from a lie.
    EXPECT_FALSE(nextState(BLINDS_STEP_UP,   MOTCMD_TIMER).snap);
    EXPECT_FALSE(nextState(BLINDS_STEP_DOWN, MOTCMD_TIMER).snap);
    EXPECT_EQ(nextState(BLINDS_STEP_UP,   MOTCMD_TIMER).next, BLINDS_IDLE);
    EXPECT_EQ(nextState(BLINDS_STEP_DOWN, MOTCMD_TIMER).next, BLINDS_IDLE);
}

TEST(MotorPolicy, AnIdleTimerOrStopChangesNothing) {
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_TIMER).next, BLINDS_IDLE);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_STOP).next,  BLINDS_IDLE);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_IDLE).next,  BLINDS_IDLE);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_IDLE).drive, Drive::None);
}

TEST(MotorPolicy, StepAndFullUseDifferentDurations) {
    // A step is a fixed nudge; a full move uses the configured travel time.
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_STEP_UP).drive,   Drive::UpStep);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_FULL_UP).drive,   Drive::UpFull);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_STEP_DOWN).drive, Drive::DownStep);
    EXPECT_EQ(nextState(BLINDS_IDLE, MOTCMD_FULL_DOWN).drive, Drive::DownFull);
}

// ---------------------------------------------------------------------------
// Where the blind is
// ---------------------------------------------------------------------------

TEST(MotorPolicy, TheUnsealHeadIsNotTravel) {
    // Opening from sealed, the bar sits at the sill while the slats spread: the
    // motor turns and the blind does not rise. Counting it as travel makes the
    // estimate run ahead of the blind for the entire move.
    EXPECT_EQ(travelMs(0,    2000), 0u);
    EXPECT_EQ(travelMs(1999, 2000), 0u);
    EXPECT_EQ(travelMs(2000, 2000), 0u);
    EXPECT_EQ(travelMs(2500, 2000), 500u);
}

TEST(MotorPolicy, WithoutSlackTravelIsJustElapsed) {
    EXPECT_EQ(travelMs(1234, 0), 1234u);
}

TEST(MotorPolicy, RadiusGrowsWhileOpeningAndShrinksWhileClosing) {
    const float delta = kFullR - kAxleR;
    const float mid_open  = radiusAfter(true,  kAxleR, delta, kOpenMs / 2, kOpenMs, kAxleR, kFullR);
    const float mid_close = radiusAfter(false, kFullR, delta, kCloseMs / 2, kCloseMs, kAxleR, kFullR);
    EXPECT_GT(mid_open, kAxleR);
    EXPECT_LT(mid_open, kFullR);
    EXPECT_LT(mid_close, kFullR);
    EXPECT_GT(mid_close, kAxleR);
}

TEST(MotorPolicy, RadiusIsClampedToThePhysicalRoll) {
    // An overrun must not produce a radius the roll cannot have — the position
    // formula squares it, so a runaway value would not merely be wrong, it
    // would be wildly wrong.
    const float delta = kFullR - kAxleR;
    EXPECT_FLOAT_EQ(radiusAfter(true,  kAxleR, delta, kOpenMs * 5, kOpenMs, kAxleR, kFullR), kFullR);
    EXPECT_FLOAT_EQ(radiusAfter(false, kFullR, delta, kCloseMs * 5, kCloseMs, kAxleR, kFullR), kAxleR);
}

TEST(MotorPolicy, AZeroDurationDoesNotDivideByZero) {
    // An unconfigured or corrupt duration must not drive the position to
    // nonsense. Checking the range alone is NOT enough: 0/0 is NaN, and a NaN
    // survives clipf untouched because both of its comparisons are false. So
    // assert non-NaN explicitly, with travel 0 as well as non-zero.
    for (uint32_t travel : {0u, 1000u}) {
        const float r = radiusAfter(true, kAxleR, 20.0f, travel, /*duration=*/0,
                                    kAxleR, kFullR);
        EXPECT_FALSE(std::isnan(r)) << "travel " << travel;
        EXPECT_GE(r, kAxleR);
        EXPECT_LE(r, kFullR);
    }
}

TEST(MotorPolicy, PositionIsZeroAtTheAxleAndRisesWithRadius) {
    EXPECT_FLOAT_EQ(positionForRadius(kAxleR, kAxleR, kThickness, kHeight), 0.0f);
    EXPECT_GT(positionForRadius(kAxleR + 5.0f, kAxleR, kThickness, kHeight), 0.0f);
    EXPECT_GT(positionForRadius(kAxleR + 10.0f, kAxleR, kThickness, kHeight),
              positionForRadius(kAxleR + 5.0f,  kAxleR, kThickness, kHeight));
}

TEST(MotorPolicy, PositionIsClampedToZeroOne) {
    EXPECT_LE(positionForRadius(1000.0f, kAxleR, kThickness, kHeight), 1.0f);
    EXPECT_GE(positionForRadius(0.0f,    kAxleR, kThickness, kHeight), 0.0f);
}

TEST(MotorPolicy, DegenerateGeometryDoesNotProduceNaN) {
    // Zero thickness or height would divide by zero. A node whose geometry was
    // never pushed must still report a usable position rather than NaN, which
    // would propagate into the beacon and into Home Assistant.
    EXPECT_FLOAT_EQ(positionForRadius(20.0f, kAxleR, 0.0f, kHeight), 0.0f);
    EXPECT_FLOAT_EQ(positionForRadius(20.0f, kAxleR, kThickness, 0.0f), 0.0f);
}

TEST(MotorPolicy, PositionIsNotLinearInTime) {
    // The whole reason the estimate integrates a radius: the blind is wound on
    // a roll, so linear speed grows as it opens. Treating position as linear in
    // time is the intuitive simplification, and it is wrong.
    const float delta = kFullR - kAxleR;
    auto posAt = [&](uint32_t ms) {
        return positionForRadius(
            radiusAfter(true, kAxleR, delta, ms, kOpenMs, kAxleR, kFullR),
            kAxleR, kThickness, kHeight);
    };
    const float quarter = posAt(kOpenMs / 4);
    const float half    = posAt(kOpenMs / 2);
    EXPECT_GT(half, quarter * 2.0f)
        << "later travel covers more distance per second than earlier travel";
}

// ---------------------------------------------------------------------------
// When to stop: the current-sense endstop
// ---------------------------------------------------------------------------

TEST(MotorPolicy, EndstopArmsOnlyNearTheEnd) {
    // A mid-travel current dip (gravity assist, load variation, commutation
    // gap) must not read as "reached the end".
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_OPENING, 0.50f, false, 0.0f));
    EXPECT_TRUE (endstopArmed(BLINDS_FULLY_OPENING, 0.90f, false, 0.0f));
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_CLOSING, 0.50f, false, 0.0f));
    EXPECT_TRUE (endstopArmed(BLINDS_FULLY_CLOSING, 0.10f, false, 0.0f));
}

TEST(MotorPolicy, TheNearEndGateAppliesInBothDirections) {
    // It was once armed for the whole of one direction, which let a dip stop
    // the blind mid-travel.
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_OPENING, kEndstopArmPos - 0.01f, false, 0.0f));
    EXPECT_TRUE (endstopArmed(BLINDS_FULLY_OPENING, kEndstopArmPos,         false, 0.0f));
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_CLOSING, (1.0f - kEndstopArmPos) + 0.01f, false, 0.0f));
    EXPECT_TRUE (endstopArmed(BLINDS_FULLY_CLOSING, (1.0f - kEndstopArmPos),         false, 0.0f));
}

TEST(MotorPolicy, EndstopNeverArmsForAnIntermediateTarget) {
    // The important one. On a target move a spurious zero would preempt the
    // target stop and snap the blind to the extreme — the user asks for 50 %
    // and gets fully open.
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_OPENING, 0.95f, /*target mode=*/true, 0.5f));
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_CLOSING, 0.05f, /*target mode=*/true, 0.5f));
}

TEST(MotorPolicy, EndstopStillArmsWhenTheTargetIsAnExtreme) {
    // A target of 1.0 IS a full open, so the endstop is the right terminator.
    EXPECT_TRUE(endstopArmed(BLINDS_FULLY_OPENING, 0.95f, true, 1.0f));
    EXPECT_TRUE(endstopArmed(BLINDS_FULLY_CLOSING, 0.05f, true, 0.0f));
}

TEST(MotorPolicy, EndstopNeverArmsWhileStepping) {
    // A step is short and never reaches an end, so a zero reading during one is
    // noise by definition.
    EXPECT_FALSE(endstopArmed(BLINDS_STEP_UP,   0.99f, false, 0.0f));
    EXPECT_FALSE(endstopArmed(BLINDS_STEP_DOWN, 0.01f, false, 0.0f));
    EXPECT_FALSE(endstopArmed(BLINDS_IDLE,      0.99f, false, 0.0f));
}

TEST(MotorPolicy, TheZeroCurrentDebounceIsMoreThanOneSample) {
    // One dip is noise; the debounce is what stops a single sample halting a
    // move halfway.
    EXPECT_GT(kZeroCurrentStopCount, 1);
}

// ---------------------------------------------------------------------------
// When to stop: the target position
// ---------------------------------------------------------------------------

TEST(MotorPolicy, TargetStopsTheMoveOncePassed) {
    EXPECT_TRUE (targetReached(BLINDS_FULLY_OPENING, 0.55f, true, 0.50f));
    EXPECT_FALSE(targetReached(BLINDS_FULLY_OPENING, 0.45f, true, 0.50f));
    EXPECT_TRUE (targetReached(BLINDS_FULLY_CLOSING, 0.45f, true, 0.50f));
    EXPECT_FALSE(targetReached(BLINDS_FULLY_CLOSING, 0.55f, true, 0.50f));
}

TEST(MotorPolicy, TargetIsIgnoredWhenNotInTargetMode) {
    // A plain open/close must run to the end, not stop at a stale target.
    EXPECT_FALSE(targetReached(BLINDS_FULLY_OPENING, 0.99f, /*target mode=*/false, 0.5f));
    EXPECT_FALSE(targetReached(BLINDS_FULLY_CLOSING, 0.01f, /*target mode=*/false, 0.5f));
}

// ---------------------------------------------------------------------------
// When to stop: the run-time backstop
// ---------------------------------------------------------------------------

TEST(MotorPolicy, TheBackstopBudgetIncludesTheSlack) {
    // Without the slack in the budget the backstop cuts the compression run at
    // the end of a close, or the un-seal head at the start of an open — the
    // blind stops short of the end on every single move.
    const uint32_t with_slack = maxRunMs(true, kOpenMs, kCloseMs, /*head=*/3000, 0);
    const uint32_t no_slack   = maxRunMs(true, kOpenMs, kCloseMs, 0, 0);
    EXPECT_GT(with_slack, no_slack);

    const uint32_t close_with_tail = maxRunMs(false, kOpenMs, kCloseMs, 0, /*tail=*/4000);
    EXPECT_GT(close_with_tail, maxRunMs(false, kOpenMs, kCloseMs, 0, 0));
}

TEST(MotorPolicy, TheBackstopAllowsMarginBeyondNominal) {
    // It must not fire at exactly the nominal duration — the motor is slower
    // under load, and a backstop that trips on every normal move would mask the
    // real terminator and log a warning every time.
    EXPECT_GT(maxRunMs(true, kOpenMs, kCloseMs, 0, 0), kOpenMs);
    EXPECT_FALSE(runTimeExceeded(kOpenMs, maxRunMs(true, kOpenMs, kCloseMs, 0, 0)));
}

TEST(MotorPolicy, TheBackstopFiresOnASufficientOverrun) {
    const uint32_t limit = maxRunMs(true, kOpenMs, kCloseMs, 0, 0);
    EXPECT_TRUE(runTimeExceeded(limit, limit));
    EXPECT_TRUE(runTimeExceeded(limit + 1, limit));
}

TEST(MotorPolicy, TheAbsoluteCeilingCannotBeRaisedByConfiguration) {
    // A mis-set duration must never drive the motor for minutes. This is the
    // guard against a stuck relay, so it deliberately ignores the computed
    // limit.
    const uint32_t absurd = maxRunMs(true, /*open=*/3600000, 0, 0, 0);
    EXPECT_GT(absurd, kAbsMaxRunMs);
    EXPECT_TRUE(runTimeExceeded(kAbsMaxRunMs, absurd))
        << "the fixed ceiling must fire even when the configured limit is higher";
}

TEST(MotorPolicy, AnOverrunOnAFullMoveSnapsToTheExtreme) {
    EXPECT_FLOAT_EQ(positionAfterOverrun(true,  false, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(positionAfterOverrun(false, false, 0.0f), 0.0f);
}

TEST(MotorPolicy, AnOverrunOnATargetMoveTrustsTheTarget) {
    // Claiming an extreme here would be a bigger lie than the target: the blind
    // is somewhere near what was asked for, not at the end.
    EXPECT_FLOAT_EQ(positionAfterOverrun(true,  true, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(positionAfterOverrun(false, true, 0.5f), 0.5f);
}

TEST(MotorPolicy, ATargetOfAnExtremeStillSnaps) {
    // Target 1.0 is a full open; it should land exactly on the extreme rather
    // than on a float that merely rounds to it.
    EXPECT_FLOAT_EQ(positionAfterOverrun(true,  true, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(positionAfterOverrun(false, true, 0.0f), 0.0f);
}

TEST(MotorPolicy, TheEndstopSnapsToTheEndBeingDrivenInto) {
    EXPECT_FLOAT_EQ(positionAfterEndstop(true),  1.0f);
    EXPECT_FLOAT_EQ(positionAfterEndstop(false), 0.0f);
}

// ---------------------------------------------------------------------------
// The stopping mechanisms must not contradict each other
// ---------------------------------------------------------------------------

TEST(MotorPolicy, OnATargetMoveOnlyTheTargetCanStopIt) {
    // The four terminators overlap, and this is the combination that matters:
    // a 50 % request must be ended by the target, never by the endstop.
    const float pos = 0.95f;
    EXPECT_FALSE(endstopArmed(BLINDS_FULLY_OPENING, pos, true, 0.5f));
    EXPECT_TRUE (targetReached(BLINDS_FULLY_OPENING, pos, true, 0.5f));
}

TEST(MotorPolicy, OnAFullMoveTheTargetNeverInterferes) {
    const float pos = 0.95f;
    EXPECT_TRUE (endstopArmed(BLINDS_FULLY_OPENING, pos, false, 0.5f));
    EXPECT_FALSE(targetReached(BLINDS_FULLY_OPENING, pos, false, 0.5f));
}

TEST(MotorPolicy, MovingAndDirectionHelpersAgreeWithTheStates) {
    EXPECT_TRUE(isMoving(BLINDS_STEP_UP));
    EXPECT_TRUE(isMoving(BLINDS_FULLY_CLOSING));
    EXPECT_FALSE(isMoving(BLINDS_IDLE));
    EXPECT_TRUE(isOpening(BLINDS_STEP_UP));
    EXPECT_TRUE(isOpening(BLINDS_FULLY_OPENING));
    EXPECT_FALSE(isOpening(BLINDS_FULLY_CLOSING));
    EXPECT_FALSE(isOpening(BLINDS_STEP_DOWN));
}

// ---------------------------------------------------------------------------
// Slat slack: a STEP must never collect it
// ---------------------------------------------------------------------------

TEST(SlatSlack, AStepDownGetsNoSealTail) {
    // The bug this exists to prevent. A step has no target position, and the
    // original condition read "no target" as "full close", so every step down
    // appended the 7 s seal tail: 3000 + 7000 - 1000 = 9 s of motor for what
    // should be a 2 s nudge. Shipped in v1.0.7; noticed in the field as "a
    // single press runs much longer than it used to".
    EXPECT_EQ(tailSlackMs(/*is_step=*/true, /*has_target=*/false, 0.0f, 7000), 0u);
    EXPECT_EQ(tailSlackMs(/*is_step=*/true, /*has_target=*/true, 0.0f, 7000), 0u);
}

TEST(SlatSlack, AStepUpFromTheSealedPositionGetsNoHead) {
    // Same class, the other direction: a step up starting from fully closed
    // would otherwise collect the un-seal head.
    EXPECT_EQ(headSlackMs(/*is_step=*/true, 0.0f, 7000), 0u);
}

TEST(SlatSlack, AFullCloseStillSeals) {
    // The feature must survive the fix — this is what the slack is FOR.
    EXPECT_EQ(tailSlackMs(false, /*has_target=*/false, 0.0f, 7000), 7000u)
        << "a close with no target runs to the sill and must compress";
    EXPECT_EQ(tailSlackMs(false, /*has_target=*/true, 0.0f, 7000), 7000u)
        << "an explicit target of 0 is also a full close";
}

TEST(SlatSlack, AnIntermediateCloseDoesNotSeal) {
    EXPECT_EQ(tailSlackMs(false, /*has_target=*/true, 0.40f, 7000), 0u)
        << "stopping at 40% never reaches the sill";
}

TEST(SlatSlack, AFullOpenFromSealedStillUnseals) {
    EXPECT_EQ(headSlackMs(false, 0.0f, 7000), 7000u);
}

TEST(SlatSlack, AnOpenFromMidTravelHasNothingToUnseal) {
    EXPECT_EQ(headSlackMs(false, 0.50f, 7000), 0u);
}

TEST(SlatSlack, ZeroConfiguredSlackIsAlwaysZero) {
    // Nodes configured without slack must be unaffected either way.
    EXPECT_EQ(tailSlackMs(false, false, 0.0f, 0), 0u);
    EXPECT_EQ(headSlackMs(false, 0.0f, 0), 0u);
}

// ---------------------------------------------------------------------------
// Final position: a move that ends must always report where it ended
// ---------------------------------------------------------------------------

TEST(FinalPosition, AStepEndingNeedsItComputed) {
    // The bug: position is integrated only while MOVING, and by the time a
    // move ends the state is already IDLE. A step does not snap, so nothing
    // else writes the position -- and a step shorter than the FSM block time
    // never woke mid-move at all. The blind moved; HA was never told.
    EXPECT_TRUE(needsFinalPosition(/*was_moving=*/true, /*now_idle=*/true,
                                   /*snapped=*/false));
}

TEST(FinalPosition, AFullMoveSnapsInsteadAndMustNotBeRecomputed) {
    // A full move ran the whole configured duration and is against the
    // physical stop. The snap is more authoritative than the integrated
    // estimate, so recomputing would REPLACE a known-good 0.0/1.0 with a
    // drifted approximation.
    EXPECT_FALSE(needsFinalPosition(true, true, /*snapped=*/true));
}

TEST(FinalPosition, NothingToDoWhileStillMoving) {
    // motionTick() owns the position during the move.
    EXPECT_FALSE(needsFinalPosition(/*was_moving=*/true, /*now_idle=*/false, false));
}

TEST(FinalPosition, NothingToDoIfItWasNeverMoving) {
    // An IDLE->IDLE transition must not rewrite the position from a stale
    // tickStartOfOperation belonging to some previous move.
    EXPECT_FALSE(needsFinalPosition(/*was_moving=*/false, /*now_idle=*/true, false));
}

// ---------------------------------------------------------------------------
// Exact stop: run time for a commanded target position
// ---------------------------------------------------------------------------

namespace {
// Reuses the fixture geometry declared at the top of this file.
const float kDeltaR = kFullR - kAxleR;
float posToR(float pos) {
    return std::sqrt(kAxleR * kAxleR + pos * kHeight * kThickness / (float) M_PI);
}
}  // namespace

TEST(ExactStop, AFullTravelTakesTheFullDuration) {
    EXPECT_NEAR(runMsForTarget(posToR(0.0f), posToR(1.0f), kDeltaR, kOpenMs),
                kOpenMs, 200);
}

TEST(ExactStop, NoMovementTakesNoTime) {
    EXPECT_EQ(runMsForTarget(posToR(0.4f), posToR(0.4f), kDeltaR, kOpenMs), 0u);
}

TEST(ExactStop, DirectionDoesNotMatter) {
    EXPECT_EQ(runMsForTarget(posToR(0.2f), posToR(0.8f), kDeltaR, kOpenMs),
              runMsForTarget(posToR(0.8f), posToR(0.2f), kDeltaR, kOpenMs));
}

TEST(ExactStop, RoundTripsThroughTheForwardModel) {
    // THE property that matters: run for the computed time, integrate forward
    // with the SAME model, and land on the target. If these two disagreed, the
    // hardware timer and the position estimate would fight each other.
    for (float target : {0.10f, 0.25f, 0.50f, 0.75f, 0.90f}) {
        const float r0 = posToR(0.0f);
        const uint32_t ms = runMsForTarget(r0, posToR(target), kDeltaR, kOpenMs);
        const float r = radiusAfter(true, r0, kDeltaR, ms, kOpenMs, kAxleR, kFullR);
        EXPECT_NEAR(positionForRadius(r, kAxleR, kThickness, kHeight), target, 0.005f)
            << "target " << target;
    }
}

TEST(ExactStop, RoundTripsFromAMidTravelStart) {
    // Starting part-open matters: the roll radius is nonlinear in position, so
    // the same run time buys different travel at different heights.
    const float r0 = posToR(0.30f);
    const uint32_t ms = runMsForTarget(r0, posToR(0.70f), kDeltaR, kOpenMs);
    const float r = radiusAfter(true, r0, kDeltaR, ms, kOpenMs, kAxleR, kFullR);
    EXPECT_NEAR(positionForRadius(r, kAxleR, kThickness, kHeight), 0.70f, 0.005f);
}

TEST(ExactStop, DegenerateGeometryIsRefused) {
    // Never return a run time from a configuration that cannot be inverted.
    EXPECT_EQ(runMsForTarget(posToR(0.0f), posToR(1.0f), 0.0f, kOpenMs), 0u);
    EXPECT_EQ(runMsForTarget(posToR(0.0f), posToR(1.0f), kDeltaR, 0), 0u);
}

// ---------------------------------------------------------------------------
// A target move must never snap to an extreme
// ---------------------------------------------------------------------------

TEST(TargetSnap, ATargetMoveEndingOnTheTimerDoesNotSnap) {
    // The regression this exists to prevent. Once runMsForTarget made the
    // timer end PARTIAL moves too, "go to 50%" ended on the timer, hit the
    // snap, and the node declared the blind FULLY OPEN. Reported from the
    // bench as "I set 50% from closed and it says fully open".
    EXPECT_FALSE(maySnapPosition(/*transition_snaps=*/true,
                                 /*mode_target_position=*/true));
}

TEST(TargetSnap, AFullMoveStillSnaps) {
    // The snap must survive for its real purpose: a move with no target ran
    // the whole configured duration and is against the physical stop, which is
    // more trustworthy than the integrated estimate.
    EXPECT_TRUE(maySnapPosition(true, /*mode_target_position=*/false));
}

TEST(TargetSnap, NoSnapRequestedMeansNoSnap) {
    // A step never snaps regardless.
    EXPECT_FALSE(maySnapPosition(/*transition_snaps=*/false, false));
    EXPECT_FALSE(maySnapPosition(false, true));
}

TEST(TargetSnap, ASuppressedSnapLeavesTheFinalPositionToBeComputed) {
    // Belt and braces: if the snap is suppressed, needsFinalPosition must then
    // be true so SOMETHING writes the position. Otherwise suppressing the snap
    // would just leave a stale value — trading a wrong answer for a frozen one.
    const bool snapping = maySnapPosition(true, /*target=*/true);
    EXPECT_TRUE(needsFinalPosition(/*was_moving=*/true, /*now_idle=*/true, snapping));
}
