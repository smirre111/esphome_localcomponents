#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// TimeSyncPolicy — how often an automatic-mode node needs its wall clock
// resynced.
//
// TimeSync is unacknowledged and fires today on every login and every wake
// beacon, for every node, regardless of mode (power-rf-review-2026-09-27.md
// finding 1). Mode B and plain interactive nodes are left exactly as they are
// by user decision (2026-09-27): Mode B's cadence is not this policy's
// concern, and neither is a node that is neither Mode B nor automatic.
//
// An AUTOMATIC-mode node is the one case this narrows. Its wall clock comes
// from nowhere else: `settimeofday()` (CmdDispatcher::handleTimeSync) is the
// only write to it, and ESP-IDF's RTC keeps it running across deep sleep at
// the crystal's own uncorrected ppm in between. Left entirely alone it would
// drift by that ppm for the node's whole life; resent every single wake it
// costs a full 17-copy burst for a correction of a few milliseconds. The user
// judged the schedule's real tolerance and settled on ONE WEEK.
// ---------------------------------------------------------------------------

namespace timesyncpolicy
{

// One week, in the same microsecond unit esp_timer_get_time() uses.
constexpr int64_t kAutoModeIntervalUs = 7LL * 24 * 3600 * 1000000;

// `elapsed_since_last_us < 0` means "never sent to this node" — the first
// TimeSync for an automatic node must go through unconditionally, or
// shouldRunAutoMode() can never evaluate the schedule at all. Every other
// mode is unthrottled, by decision, not by omission.
constexpr bool shouldSend(bool auto_mode, int64_t elapsed_since_last_us)
{
    if (!auto_mode)
        return true;
    return elapsed_since_last_us < 0 || elapsed_since_last_us >= kAutoModeIntervalUs;
}

}  // namespace timesyncpolicy
