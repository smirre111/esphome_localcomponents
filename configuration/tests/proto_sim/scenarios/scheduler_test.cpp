// P3 — schedule arithmetic.
//
// This is the part of automatic mode that is easy to get subtly wrong and
// expensive to debug once it is running on a battery node behind a closed
// blind: an off-by-one weekday, a tie broken the wrong way, or a "next"
// occurrence that returns *now* would all manifest as a blind that moves at the
// wrong time, days later, with no log to hand.
//
// Scheduler.cpp is dependency-free, so these tests compile the exact production
// source with no shims.

#include <gtest/gtest.h>

#include "Scheduler.h"

#include <ctime>
#include <string>

using namespace sched;

namespace {

// 2026-08-17 00:00:00 UTC is a MONDAY. All fixtures build from it so weekday
// expectations are readable rather than magic numbers.
constexpr uint64_t kMonMidnightUtc = 1786924800ULL;
constexpr int32_t  kCEST           = 7200;   // UTC+2

// Render a UTC epoch as local wall time, for failure messages that a human can
// actually diagnose.
std::string local_str(uint64_t utc, int32_t offset) {
    if (utc == 0) return "(none)";
    const std::time_t t = static_cast<std::time_t>(utc) + offset;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[40];
    strftime(buf, sizeof(buf), "%a %Y-%m-%d %H:%M", &tmv);
    return buf;
}

Entry mk(uint16_t minute, uint8_t days, uint8_t action = ACTION_OPEN,
         bool enabled = true, uint8_t pct = 0) {
    Entry e;
    e.minuteOfDay = minute;
    e.dayMask     = days;
    e.action      = action;
    e.positionPct = pct;
    e.enabled     = enabled;
    return e;
}

constexpr uint16_t hm(int h, int m) { return static_cast<uint16_t>(h * 60 + m); }

} // namespace

// ---------------------------------------------------------------------------
// Local-time primitives
// ---------------------------------------------------------------------------

TEST(SchedulerBasics, WeekdayBitIsMondayFirstNotCConvention) {
    // tm_wday has Sunday = 0; our bit 0 is Monday. Getting this backwards is
    // the classic way a "weekdays" schedule fires on Sunday.
    EXPECT_EQ(local_weekday_bit(kMonMidnightUtc, 0), DAY_MON);
    EXPECT_EQ(local_weekday_bit(kMonMidnightUtc + 1 * 86400, 0), DAY_TUE);
    EXPECT_EQ(local_weekday_bit(kMonMidnightUtc + 5 * 86400, 0), DAY_SAT);
    EXPECT_EQ(local_weekday_bit(kMonMidnightUtc + 6 * 86400, 0), DAY_SUN);
}

TEST(SchedulerBasics, OffsetCanPushTheLocalDayForward) {
    // 23:30 UTC Monday is 01:30 TUESDAY local at UTC+2. An entry scheduled for
    // Tuesday must therefore be considered.
    const uint64_t utc = kMonMidnightUtc + hm(23, 30) * 60;
    EXPECT_EQ(local_weekday_bit(utc, kCEST), DAY_TUE);
    EXPECT_EQ(local_minute_of_day(utc, kCEST), hm(1, 30));
}

TEST(SchedulerBasics, NegativeOffsetCanPushTheLocalDayBackward) {
    // 00:30 UTC Monday is 19:30 SUNDAY local at UTC-5.
    const uint64_t utc = kMonMidnightUtc + hm(0, 30) * 60;
    EXPECT_EQ(local_weekday_bit(utc, -18000), DAY_SUN);
    EXPECT_EQ(local_minute_of_day(utc, -18000), hm(19, 30));
}

TEST(SchedulerBasics, MidnightIsTheStartOfTheLOCALDay) {
    const uint64_t utc = kMonMidnightUtc + hm(23, 30) * 60;   // Tue 01:30 local
    const uint64_t mid = local_midnight_utc(utc, kCEST);
    EXPECT_EQ(local_minute_of_day(mid, kCEST), 0);
    EXPECT_EQ(local_weekday_bit(mid, kCEST), DAY_TUE);
}

// ---------------------------------------------------------------------------
// next_occurrence
// ---------------------------------------------------------------------------

TEST(SchedulerNext, FindsLaterToday) {
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};
    const uint64_t now = kMonMidnightUtc + hm(5, 0) * 60;   // 05:00 UTC
    int which = -1;
    const uint64_t next = next_occurrence(e, 1, now, /*offset=*/0, &which);

    EXPECT_EQ(which, 0);
    EXPECT_EQ(next, kMonMidnightUtc + hm(7, 30) * 60)
        << "got " << local_str(next, 0);
}

TEST(SchedulerNext, RollsToTomorrowWhenTodaysTimeHasPassed) {
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};
    const uint64_t now = kMonMidnightUtc + hm(9, 0) * 60;
    const uint64_t next = next_occurrence(e, 1, now, 0);
    EXPECT_EQ(next, kMonMidnightUtc + 86400 + hm(7, 30) * 60)
        << "got " << local_str(next, 0);
}

TEST(SchedulerNext, IsStrictlyAfterNowNotAtNow) {
    // If "now" were accepted, a node waking exactly on its event would compute
    // a zero-length sleep and could spin re-firing the same entry.
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};
    const uint64_t now = kMonMidnightUtc + hm(7, 30) * 60;   // exactly the entry
    const uint64_t next = next_occurrence(e, 1, now, 0);
    EXPECT_EQ(next, kMonMidnightUtc + 86400 + hm(7, 30) * 60)
        << "must skip to the next day, got " << local_str(next, 0);
}

TEST(SchedulerNext, SkipsDaysNotInTheMask) {
    // Saturday-only, asked on a Monday: must land on the coming Saturday.
    Entry e[] = {mk(hm(9, 0), DAY_SAT)};
    const uint64_t now  = kMonMidnightUtc + hm(6, 0) * 60;
    const uint64_t next = next_occurrence(e, 1, now, 0);
    EXPECT_EQ(local_weekday_bit(next, 0), DAY_SAT) << "got " << local_str(next, 0);
    EXPECT_EQ(next, kMonMidnightUtc + 5 * 86400 + hm(9, 0) * 60);
}

TEST(SchedulerNext, WrapsAcrossTheWeekBoundary) {
    // Monday-only, asked on Monday afternoon: next is a week later. This is the
    // case that needs the 8-day search window — a 7-day one would just miss it.
    Entry e[] = {mk(hm(9, 0), DAY_MON)};
    const uint64_t now  = kMonMidnightUtc + hm(15, 0) * 60;
    const uint64_t next = next_occurrence(e, 1, now, 0);
    EXPECT_EQ(next, kMonMidnightUtc + 7 * 86400 + hm(9, 0) * 60)
        << "got " << local_str(next, 0);
}

TEST(SchedulerNext, PicksEarliestAcrossEntries) {
    Entry e[] = {
        mk(hm(21, 0), DAY_ALL),
        mk(hm(7, 30), DAY_ALL),
        mk(hm(12, 0), DAY_ALL),
    };
    const uint64_t now = kMonMidnightUtc + hm(5, 0) * 60;
    int which = -1;
    const uint64_t next = next_occurrence(e, 3, now, 0, &which);
    EXPECT_EQ(which, 1) << "the 07:30 entry is index 1";
    EXPECT_EQ(next, kMonMidnightUtc + hm(7, 30) * 60);
}

TEST(SchedulerNext, TieGoesToTheLowestIndex) {
    // Documented conflict rule: same minute -> lowest slot wins. Without a
    // deterministic rule the node and the hub's mirror could disagree about
    // which entry ran.
    Entry e[] = {
        mk(hm(8, 0), DAY_ALL, ACTION_OPEN),
        mk(hm(8, 0), DAY_ALL, ACTION_CLOSE),
    };
    const uint64_t now = kMonMidnightUtc;
    int which = -1;
    next_occurrence(e, 2, now, 0, &which);
    EXPECT_EQ(which, 0);
}

TEST(SchedulerNext, IgnoresDisabledAndEmptyMaskEntries) {
    Entry e[] = {
        mk(hm(6, 0), DAY_ALL, ACTION_OPEN, /*enabled=*/false),  // disabled
        mk(hm(7, 0), 0),                                        // no days set
        mk(hm(8, 0), DAY_ALL),                                  // the real one
    };
    const uint64_t now = kMonMidnightUtc;
    int which = -1;
    const uint64_t next = next_occurrence(e, 3, now, 0, &which);
    EXPECT_EQ(which, 2);
    EXPECT_EQ(next, kMonMidnightUtc + hm(8, 0) * 60);
}

TEST(SchedulerNext, ReturnsZeroWhenNothingCanEverFire) {
    // Q9's runtime counterpart: no usable entry means auto mode has nothing to
    // sleep towards, and the caller must stay interactive rather than sleep
    // forever.
    Entry none[] = {
        mk(hm(6, 0), DAY_ALL, ACTION_OPEN, /*enabled=*/false),
        mk(hm(7, 0), 0),
    };
    int which = 99;
    EXPECT_EQ(next_occurrence(none, 2, kMonMidnightUtc, 0, &which), 0u);
    EXPECT_EQ(which, -1);
    EXPECT_EQ(next_occurrence(nullptr, 0, kMonMidnightUtc, 0), 0u);
}

TEST(SchedulerNext, RespectsLocalOffsetForBothTimeAndWeekday) {
    // 07:30 local at UTC+2 is 05:30 UTC. Asked at 05:00 UTC Monday it is still
    // ahead; asked at 06:00 UTC it has passed and must roll to Tuesday.
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};

    const uint64_t before = next_occurrence(e, 1, kMonMidnightUtc + hm(5, 0) * 60, kCEST);
    EXPECT_EQ(before, kMonMidnightUtc + hm(5, 30) * 60) << local_str(before, kCEST);

    const uint64_t after = next_occurrence(e, 1, kMonMidnightUtc + hm(6, 0) * 60, kCEST);
    EXPECT_EQ(after, kMonMidnightUtc + 86400 + hm(5, 30) * 60) << local_str(after, kCEST);
}

TEST(SchedulerNext, WeekdayMaskIsEvaluatedInLocalTimeNotUtc) {
    // The subtle one. A Tuesday-only 00:30 local entry, at UTC+2, actually
    // fires at 22:30 UTC on MONDAY. Evaluating the mask against the UTC weekday
    // would skip it entirely and the blind would never move.
    Entry e[] = {mk(hm(0, 30), DAY_TUE)};
    const uint64_t now  = kMonMidnightUtc + hm(12, 0) * 60;   // Monday midday UTC
    const uint64_t next = next_occurrence(e, 1, now, kCEST);

    EXPECT_EQ(next, kMonMidnightUtc + hm(22, 30) * 60)
        << "expected Mon 22:30 UTC == Tue 00:30 local, got " << local_str(next, kCEST);
    EXPECT_EQ(local_weekday_bit(next, kCEST), DAY_TUE);
}

TEST(SchedulerNext, HandlesLastMinuteOfDay) {
    Entry e[] = {mk(1439, DAY_ALL)};   // 23:59
    const uint64_t next = next_occurrence(e, 1, kMonMidnightUtc, 0);
    EXPECT_EQ(next, kMonMidnightUtc + 1439 * 60);
    EXPECT_EQ(local_minute_of_day(next, 0), 1439);
}

TEST(SchedulerNext, CapsCountAtMaxEntries) {
    // Defensive: a corrupt persisted count must not walk off the array.
    Entry e[kMaxEntries];
    for (auto &x : e) x = mk(hm(8, 0), DAY_ALL);
    EXPECT_NO_FATAL_FAILURE(next_occurrence(e, 999, kMonMidnightUtc, 0));
}

// ---------------------------------------------------------------------------
// last_missed (catch-up, I3)
// ---------------------------------------------------------------------------

TEST(SchedulerMissed, FindsAnEntryMissedWhileAsleep) {
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};
    const uint64_t from = kMonMidnightUtc + hm(6, 0) * 60;
    const uint64_t to   = kMonMidnightUtc + hm(9, 0) * 60;
    int which = -1;
    const uint64_t miss = last_missed(e, 1, from, to, 0, &which);
    EXPECT_EQ(which, 0);
    EXPECT_EQ(miss, kMonMidnightUtc + hm(7, 30) * 60) << local_str(miss, 0);
}

TEST(SchedulerMissed, ReturnsTheLATESTMissedNotTheFirst) {
    // Replaying the earlier one after the later one would leave the blind in
    // the wrong end state — the opposite of next_occurrence's tie rule.
    Entry e[] = {
        mk(hm(7, 0), DAY_ALL, ACTION_OPEN),
        mk(hm(9, 0), DAY_ALL, ACTION_CLOSE),
    };
    const uint64_t from = kMonMidnightUtc + hm(6, 0) * 60;
    const uint64_t to   = kMonMidnightUtc + hm(10, 0) * 60;
    int which = -1;
    const uint64_t miss = last_missed(e, 2, from, to, 0, &which);
    EXPECT_EQ(which, 1) << "the 09:00 CLOSE is the one that should be replayed";
    EXPECT_EQ(miss, kMonMidnightUtc + hm(9, 0) * 60);
}

TEST(SchedulerMissed, WindowIsExclusiveAtStartInclusiveAtEnd) {
    // Exclusive start: an entry that already ran right before we slept must not
    // be replayed on wake.
    Entry e[] = {mk(hm(7, 0), DAY_ALL)};
    const uint64_t at = kMonMidnightUtc + hm(7, 0) * 60;

    EXPECT_EQ(last_missed(e, 1, at, at + 3600, 0), 0u)
        << "an entry exactly at the window start must not be replayed";
    EXPECT_EQ(last_missed(e, 1, at - 3600, at, 0), at)
        << "an entry exactly at the window end IS due";
}

TEST(SchedulerMissed, ReturnsZeroForAnEmptyOrInvertedWindow) {
    Entry e[] = {mk(hm(7, 0), DAY_ALL)};
    const uint64_t t = kMonMidnightUtc + hm(8, 0) * 60;
    EXPECT_EQ(last_missed(e, 1, t, t, 0), 0u);
    EXPECT_EQ(last_missed(e, 1, t, t - 3600, 0), 0u);
}

TEST(SchedulerMissed, SpansMultipleDaysAndRespectsTheDayMask) {
    // Node was off from Monday to Thursday. Only weekday entries count, and the
    // latest of them wins.
    Entry e[] = {mk(hm(8, 0), DAY_MON | DAY_WED)};
    const uint64_t from = kMonMidnightUtc + hm(6, 0) * 60;
    const uint64_t to   = kMonMidnightUtc + 3 * 86400 + hm(12, 0) * 60;  // Thu
    const uint64_t miss = last_missed(e, 1, from, to, 0);
    EXPECT_EQ(miss, kMonMidnightUtc + 2 * 86400 + hm(8, 0) * 60)
        << "expected Wednesday 08:00, got " << local_str(miss, 0);
    EXPECT_EQ(local_weekday_bit(miss, 0), DAY_WED);
}

TEST(SchedulerMissed, IgnoresDisabledEntries) {
    Entry e[] = {mk(hm(7, 0), DAY_ALL, ACTION_OPEN, /*enabled=*/false)};
    EXPECT_EQ(last_missed(e, 1, kMonMidnightUtc, kMonMidnightUtc + 86400, 0), 0u);
}

TEST(SchedulerMissed, RespectsLocalOffset) {
    // 07:30 local at UTC+2 == 05:30 UTC. A window of 05:00..06:00 UTC contains
    // it; 06:00..07:00 UTC does not.
    Entry e[] = {mk(hm(7, 30), DAY_ALL)};
    EXPECT_EQ(last_missed(e, 1, kMonMidnightUtc + hm(5, 0) * 60,
                          kMonMidnightUtc + hm(6, 0) * 60, kCEST),
              kMonMidnightUtc + hm(5, 30) * 60);
    EXPECT_EQ(last_missed(e, 1, kMonMidnightUtc + hm(6, 0) * 60,
                          kMonMidnightUtc + hm(7, 0) * 60, kCEST), 0u);
}
