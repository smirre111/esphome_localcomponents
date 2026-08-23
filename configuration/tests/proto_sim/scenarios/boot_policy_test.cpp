// Boot policy — reset classification and the reference-move decision.
//
// Regression cover for a live failure: node 2 executed its scheduled CLOSE
// correctly (Position: 0%), restarted ~1 minute later reporting WakeReason
// BOOT, and the power-on branch then issued MOTCMD_FULL_UP — driving the blind
// straight back to 100% and silently undoing the schedule.
//
// The reference move exists to rebuild a position reference after RTC RAM is
// lost.  It was gated on `powerOnBoot`, which is true for EVERY reset that is
// not a deep-sleep wake — including ESP_RST_SW, ESP_RST_PANIC and the
// watchdogs, all of which keep RTC RAM and therefore keep a perfectly good
// position.  Moving the blind there is not a harmless default: it destroys the
// state the schedule just established.
//
// (Measured on this ESP32: an EN-pin reset from a debugger or serial monitor
// toggling DTR/RTS reports ESP_RST_POWERON and genuinely DOES clear RTC RAM, so
// the reference move is correct in that case — which is why the decision is
// made on the marker rather than on a list of reset reasons.)

#include <gtest/gtest.h>

#include "BootPolicy.h"

using namespace bootpolicy;

// A plausible-looking but unwritten RTC word: what a true power-on leaves.
static constexpr uint32_t kGarbage = 0xDEADBEEF;

// ---------------------------------------------------------------------------
// rtcRamValid
// ---------------------------------------------------------------------------

TEST(BootPolicy, RtcRamValidOnlyForWrittenMarkers) {
  EXPECT_TRUE(rtcRamValid(kSleepMarkerRunning));
  EXPECT_TRUE(rtcRamValid(kSleepMarkerSleeping));

  // Anything the firmware did not write means the RTC domain lost power.
  EXPECT_FALSE(rtcRamValid(kGarbage));
  EXPECT_FALSE(rtcRamValid(0));
  EXPECT_FALSE(rtcRamValid(0xFFFFFFFF));
  // Near-misses must not pass: the marker is a claim about power continuity.
  EXPECT_FALSE(rtcRamValid(kSleepMarkerRunning ^ 1u));
  EXPECT_FALSE(rtcRamValid(kModeMarkerAuto));
}

// ---------------------------------------------------------------------------
// needsReferenceMove — the decision that broke the blind
// ---------------------------------------------------------------------------

TEST(BootPolicy, ColdBootRebuildsPositionReference) {
  // True power-on: RTC RAM is garbage, the stored position is meaningless, so
  // the endstop move is the only way to know where the blind is.
  EXPECT_TRUE(needsReferenceMove(/*power_on_boot=*/true,
                                 /*resumed_session=*/false, kGarbage));
}

TEST(BootPolicy, ResetWithRtcRamIntactSkipsReferenceMove) {
  // THE REGRESSION.  esp_restart / panic / watchdog all yield power_on_boot ==
  // true, but RTC RAM survived, so the position is known and must be kept.
  EXPECT_FALSE(needsReferenceMove(true, false, kSleepMarkerRunning));

  // Same when the reset caught us on the way into sleep — which is precisely
  // the post-scheduled-event window where the damage was observed.
  EXPECT_FALSE(needsReferenceMove(true, false, kSleepMarkerSleeping));
}

TEST(BootPolicy, DeepSleepWakeNeverMoves) {
  // A wake is not a boot; position was never lost, whatever the marker says.
  EXPECT_FALSE(needsReferenceMove(false, false, kSleepMarkerSleeping));
  EXPECT_FALSE(needsReferenceMove(false, false, kSleepMarkerRunning));
  // Even a garbage marker: power_on_boot == false is authoritative on its own.
  EXPECT_FALSE(needsReferenceMove(false, false, kGarbage));
}

TEST(BootPolicy, ResumedSessionNeverMoves) {
  // F-5 crash-resume restored the position from RTC before this point; moving
  // would discard it. Holds even if the marker were somehow unset.
  EXPECT_FALSE(needsReferenceMove(true, true, kSleepMarkerRunning));
  EXPECT_FALSE(needsReferenceMove(true, true, kGarbage));
}

// ---------------------------------------------------------------------------
// resetWhileEnteringSleep — the forensic marker
// ---------------------------------------------------------------------------

TEST(BootPolicy, NormalWakeIsNotFlaggedAsAFailedSleep) {
  // Marker armed, and we came back via a deep-sleep wake: the expected path.
  EXPECT_FALSE(resetWhileEnteringSleep(kSleepMarkerSleeping,
                                       /*reset_was_deepsleep=*/true));
}

TEST(BootPolicy, ResetDuringSleepEntryIsFlagged) {
  // We armed the marker immediately before esp_deep_sleep_start() and yet came
  // back by some other route — the node reset on its way into sleep.
  EXPECT_TRUE(resetWhileEnteringSleep(kSleepMarkerSleeping, false));
}

TEST(BootPolicy, ResetWhileAwakeIsNotAFailedSleep) {
  EXPECT_FALSE(resetWhileEnteringSleep(kSleepMarkerRunning, false));
  // Cold boot: nothing can be concluded, and claiming a failed sleep entry
  // from uninitialised RTC RAM would be a false alarm on every power-on.
  EXPECT_FALSE(resetWhileEnteringSleep(kGarbage, false));
}

// ---------------------------------------------------------------------------
// wasAutoMode — mode across a reset
// ---------------------------------------------------------------------------

TEST(BootPolicy, PreviousModeIsReadableAcrossAReset) {
  EXPECT_TRUE(wasAutoMode(kSleepMarkerRunning, kModeMarkerAuto));
  EXPECT_TRUE(wasAutoMode(kSleepMarkerSleeping, kModeMarkerAuto));
  EXPECT_FALSE(wasAutoMode(kSleepMarkerRunning, kModeMarkerInteractive));
}

TEST(BootPolicy, PreviousModeIsNotTrustedWhenRtcRamWasLost) {
  // Without power continuity the mode word is uninitialised; reporting AUTO
  // from garbage would attribute a cold boot to automatic operation.
  EXPECT_FALSE(wasAutoMode(kGarbage, kModeMarkerAuto));
  EXPECT_FALSE(wasAutoMode(kGarbage, kGarbage));
}

// ---------------------------------------------------------------------------
// The observed failure, end to end
// ---------------------------------------------------------------------------

// app_main() overwrites the RTC sleep marker early, to record that this run is
// awake. Every decision below that point must therefore read a LATCHED copy of
// the boot-time value, not the live variable.
//
// The first build of this fix did not, and hardware caught it immediately: a
// genuine cold boot (rtc_ram=LOST) logged "position restored, skipping FULL_UP"
// and left the blind with no position reference at all — the exact inverse of
// the bug being fixed. This models both orderings so the requirement is
// explicit rather than a comment someone can quietly drop.
TEST(BootPolicy, ReferenceMoveMustUseTheLatchedBootMarker) {
  uint32_t rtc_marker = kGarbage;  // cold boot: RTC RAM was lost

  const uint32_t latched = rtc_marker;   // what app_main() must capture
  rtc_marker = kSleepMarkerRunning;      // ...before marking this run awake

  // Correct: the cold boot is still visible, so the reference move happens.
  EXPECT_TRUE(needsReferenceMove(true, false, latched));

  // Wrong: reading the live variable hides the cold boot entirely.
  EXPECT_FALSE(needsReferenceMove(true, false, rtc_marker));
}

TEST(BootPolicy, ScheduledCloseSurvivesAnExternalReset) {
  // Node is in auto mode, has just executed the 10:42 CLOSE, and is entering
  // deep sleep when something pulls the reset line.
  const uint32_t sleep_marker = kSleepMarkerSleeping;
  const uint32_t mode_marker = kModeMarkerAuto;

  // The boot path must be able to say what happened...
  EXPECT_TRUE(resetWhileEnteringSleep(sleep_marker, /*deepsleep=*/false));
  EXPECT_TRUE(wasAutoMode(sleep_marker, mode_marker));

  // ...and, crucially, must leave the blind where the schedule put it.
  EXPECT_FALSE(needsReferenceMove(/*power_on_boot=*/true,
                                  /*resumed_session=*/false, sleep_marker));
}
