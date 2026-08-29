// =============================================================================
// VENDORED COPY — DO NOT EDIT HERE.
//
// Source of truth: BlindsESP/main/include/Scheduler.h  (+ Scheduler.cpp)
// Sync with:       BlindsESP/proto/regen_stubs.sh
// Guarded by:      ctest `scheduler_drift_hub_vs_node`
//
// The hub needs this to predict when an auto-mode node will next wake, so it
// can stop bursting LoginMsg at a node that cannot hear it. That prediction
// must use the NODE'S OWN next-occurrence logic, not a hub re-implementation —
// a second implementation would drift and the hub would confidently transmit
// into silence, which is exactly the bug this is fixing.
// =============================================================================

#pragma once

// Pure schedule arithmetic for automatic mode.
//
// Deliberately dependency-free (only <stdint.h> / <time.h>) so it compiles
// identically on the ESP32 and on a host, and so the calendar logic — the part
// that is easy to get subtly wrong and expensive to debug on a sleeping battery
// node — can be exercised exhaustively off-target.
//
// Time model
// ----------
// Entries are LOCAL times: "07:30 on weekdays" must stay 07:30 across a DST
// change. The node holds a UTC epoch plus the hub-supplied local offset, and
// all conversion happens here as `local = utc + offset`. There is deliberately
// no timezone database and no setenv("TZ") — the hub has already resolved DST
// (and sunrise/sunset, and jitter) into absolute local minutes-of-day before
// the schedule ever reaches the node.
//
// A consequence worth knowing: across a DST transition the node's offset is
// stale until the hub pushes a corrected TimeSync, so entries land an hour off
// until then. That is the tradeoff for keeping the node free of tz data, and
// why the hub pushes TimeSync on every wake.

#include <stdint.h>
#include <time.h>

namespace sched
{

// Mirrors SchedAction in blinds.proto. Kept as a plain enum so this header has
// no dependency on the generated protobuf stubs.
enum Action : uint8_t
{
    ACTION_OPEN     = 0,
    ACTION_CLOSE    = 1,
    ACTION_STOP     = 2,
    ACTION_POSITION = 3,
};

// Day bits. Monday is bit 0 — NOT the C convention (tm_wday has Sunday = 0),
// so conversion is always via local_weekday_bit() below rather than tm_wday.
enum DayBit : uint8_t
{
    DAY_MON = 1u << 0,
    DAY_TUE = 1u << 1,
    DAY_WED = 1u << 2,
    DAY_THU = 1u << 3,
    DAY_FRI = 1u << 4,
    DAY_SAT = 1u << 5,
    DAY_SUN = 1u << 6,
    DAY_ALL = 0x7Fu,
};

static constexpr int      kMaxEntries   = 8;
static constexpr uint16_t kMinutesPerDay = 1440;
// How far ahead nextOccurrence() will search. Eight days guarantees every
// weekday is covered even when "today" is already past the entry's time.
static constexpr int      kSearchDays   = 8;

struct Entry
{
    uint16_t minuteOfDay{0};   // 0..1439, LOCAL
    uint8_t  dayMask{0};       // DayBit combination; 0 = never fires
    uint8_t  action{ACTION_OPEN};
    uint8_t  positionPct{0};   // only meaningful for ACTION_POSITION
    bool     enabled{false};

    bool valid() const
    {
        return this->enabled && this->dayMask != 0 &&
               this->minuteOfDay < kMinutesPerDay;
    }
};

// Local weekday of a UTC epoch, as a DayBit.
uint8_t local_weekday_bit(uint64_t utc_epoch, int32_t utc_offset);

// Local minute-of-day of a UTC epoch (0..1439).
uint16_t local_minute_of_day(uint64_t utc_epoch, int32_t utc_offset);

// UTC epoch of local midnight on the day containing `utc_epoch`.
uint64_t local_midnight_utc(uint64_t utc_epoch, int32_t utc_offset);

// Next firing STRICTLY AFTER now_utc, as a UTC epoch. Returns 0 when no entry
// can ever fire (none enabled, or every dayMask empty).
//
// Ties — two entries at the same minute — resolve to the LOWEST index, so the
// result is deterministic and matches the documented rule.
uint64_t next_occurrence(const Entry *entries, int count,
                         uint64_t now_utc, int32_t utc_offset,
                         int *which_out = nullptr);

// The LATEST firing in the window (from_utc, to_utc] — used for catch-up after
// a reboot or an overrun sleep. Returns 0 when nothing was due.
//
// Only the latest matters: if both the 07:00 open and the 09:00 close were
// missed, replaying the open first would leave the blind wrong, and replaying
// both wastes motor runtime to reach the same end state.
uint64_t last_missed(const Entry *entries, int count,
                     uint64_t from_utc, uint64_t to_utc, int32_t utc_offset,
                     int *which_out = nullptr);

} // namespace sched
