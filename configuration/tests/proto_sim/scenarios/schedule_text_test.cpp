// ScheduleText — the compact schedule format.
//
// The entity version of schedule editing could not be mistyped: a time picker
// only yields valid times, a select only yields valid actions. That was its main
// justification, and it is what this format gives up. So it only wins if bad
// input is REJECTED WITH A REASON rather than half-applied — a parser that
// silently drops an entry would be strictly worse than the twenty controls it
// replaces.
//
// Hence the emphasis below on failure cases and on "nothing is applied when the
// input is bad", not just on the happy path.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ScheduleText.h"

using namespace scheduletext;

namespace {

constexpr uint8_t kDaily    = 0b1111111;
constexpr uint8_t kWeekdays = 0b0011111;
constexpr uint8_t kWeekend  = 0b1100000;

std::string rendered(const ParseResult &r) {
    char buf[kTextLen];
    format(r.entries, r.count, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// The wire enum — pinned, because assuming it was dense caused a real bug
// ---------------------------------------------------------------------------

TEST(ScheduleText, ActionConstantsMatchTheWireEnum) {
    // blinds.proto: SCHED_OPEN=0 SCHED_CLOSE=1 SCHED_STOP=2 SCHED_POSITION=3.
    // The gap is the point: POSITION is 3, not 2. The first version of this
    // header (and of the HA action select) assumed a dense 0,1,2 and so wrote
    // STOP whenever the user asked for a position — a scheduled position move
    // would have halted the blind wherever it happened to be.
    EXPECT_EQ(kActionOpen, 0);
    EXPECT_EQ(kActionClose, 1);
    EXPECT_EQ(kActionStop, 2);
    EXPECT_EQ(kActionPosition, 3);
}

TEST(ScheduleText, PositionParsesToThreeNotTwo) {
    const auto r = parse("12:00 daily position:40");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entries[0].action, 3);
}

TEST(ScheduleText, StopIsExpressible) {
    const auto r = parse("12:00 daily stop");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entries[0].action, 2);
    EXPECT_EQ(rendered(r), "12:00 daily stop");
}

// ---------------------------------------------------------------------------
// The format people will actually type
// ---------------------------------------------------------------------------

TEST(ScheduleText, ParsesTheProductionSchedule) {
    const auto r = parse("06:00 daily open; 21:45 daily close");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.count, 2);
    EXPECT_EQ(r.entries[0].minuteOfDay, 360);
    EXPECT_EQ(r.entries[0].dayMask, kDaily);
    EXPECT_EQ(r.entries[0].action, kActionOpen);
    EXPECT_EQ(r.entries[1].minuteOfDay, 21 * 60 + 45);
    EXPECT_EQ(r.entries[1].action, kActionClose);
}

TEST(ScheduleText, EmptyMeansNoEntries) {
    // Clearing the schedule must be expressible, and must not be an error —
    // otherwise there is no way to remove the last entry.
    const auto r = parse("");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.count, 0);
}

TEST(ScheduleText, WeekdayAndWeekendPresets) {
    const auto r = parse("06:00 weekdays open; 07:30 weekend open");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entries[0].dayMask, kWeekdays);
    EXPECT_EQ(r.entries[1].dayMask, kWeekend);
}

TEST(ScheduleText, ArbitraryDayListsWork) {
    // The days SELECT could not express this — it offered presets and single
    // days only. The text format is strictly more capable here.
    const auto r = parse("08:00 mon,thu open");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entries[0].dayMask, (1 << 0) | (1 << 3));
}

TEST(ScheduleText, PositionCarriesItsPercentage) {
    // The entity version shipped without a position control at all, so this
    // action was selectable and then unusable. Here it is part of the syntax.
    const auto r = parse("12:00 sat position:40");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.entries[0].action, kActionPosition);
    EXPECT_EQ(r.entries[0].positionPct, 40);
    EXPECT_EQ(r.entries[0].dayMask, 1 << 5);
}

TEST(ScheduleText, ToleratesCaseAndSpacing) {
    const auto r = parse("  06:00   DAILY   Open ;;  21:45 Daily CLOSE  ");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.count, 2);
    EXPECT_EQ(r.entries[0].action, kActionOpen);
    EXPECT_EQ(r.entries[1].action, kActionClose);
}

// ---------------------------------------------------------------------------
// Rejection — the half that justifies the format
// ---------------------------------------------------------------------------

TEST(ScheduleText, RejectsAnImpossibleTime) {
    const auto r = parse("25:00 daily open");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::strstr(r.error, "25:00"), nullptr)
        << "the message must name the offending token, or the user cannot find it";
}

TEST(ScheduleText, RejectsAnImpossibleMinute) {
    EXPECT_FALSE(parse("06:99 daily open").ok);
}

TEST(ScheduleText, RejectsAMalformedTime) {
    EXPECT_FALSE(parse("0600 daily open").ok);
    EXPECT_FALSE(parse("6h00 daily open").ok);
}

TEST(ScheduleText, RejectsAnUnknownDay) {
    const auto r = parse("06:00 funday open");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::strstr(r.error, "funday"), nullptr);
}

TEST(ScheduleText, RejectsAnUnknownAction) {
    const auto r = parse("06:00 daily wiggle");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::strstr(r.error, "wiggle"), nullptr);
}

TEST(ScheduleText, RejectsPositionWithoutAPercentage) {
    const auto r = parse("12:00 daily position");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::strstr(r.error, "position"), nullptr);
}

TEST(ScheduleText, RejectsAPercentageAbove100) {
    EXPECT_FALSE(parse("12:00 daily position:140").ok);
}

TEST(ScheduleText, RejectsAMissingField) {
    EXPECT_FALSE(parse("06:00 daily").ok);   // no action
    EXPECT_FALSE(parse("06:00").ok);          // no days either
}

TEST(ScheduleText, RejectsMoreThanEightEntries) {
    // The wire format carries at most 8; accepting a ninth and dropping it
    // would be the silent data loss this whole test file exists to prevent.
    std::string s;
    for (int i = 0; i < 9; i++)
        s += "0" + std::to_string(i) + ":00 daily open; ";
    EXPECT_FALSE(parse(s.c_str()).ok);
}

TEST(ScheduleText, NothingIsAppliedWhenAnyEntryIsBad) {
    // The critical property. A parse that returned the good entries and dropped
    // the bad one would silently change the schedule to something the user never
    // asked for — worse than rejecting the whole edit.
    const auto r = parse("06:00 daily open; 25:00 daily close");
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.count, 0)
        << "a failed parse must apply nothing, not the entries before the error";
}

// ---------------------------------------------------------------------------
// Canonical round-trip — what gets echoed back
// ---------------------------------------------------------------------------

TEST(ScheduleText, RendersTheCanonicalForm) {
    const auto r = parse("6:0 D Open");   // deliberately sloppy but unambiguous
    if (r.ok) {
        // If sloppy input is accepted at all, the echo must show what was really
        // stored, so the user can see how it was interpreted.
        EXPECT_EQ(rendered(r), "06:00 daily open");
    }
}

TEST(ScheduleText, RoundTripsWithoutDrift) {
    const char *original = "06:00 weekdays open; 07:30 weekend open; 21:45 daily close";
    const auto first = parse(original);
    ASSERT_TRUE(first.ok) << first.error;
    const std::string once = rendered(first);
    const auto second = parse(once.c_str());
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(once, rendered(second)) << "re-parsing the canonical form must be a fixed point";
    EXPECT_EQ(once, original);
}

TEST(ScheduleText, RendersArbitraryMasksAndPositions) {
    const auto r = parse("08:00 mon,thu position:5");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(rendered(r), "08:00 mon,thu position:5");
}

TEST(ScheduleText, RendersPositionBoundaries) {
    EXPECT_EQ(rendered(parse("00:00 daily position:0")), "00:00 daily position:0");
    EXPECT_EQ(rendered(parse("23:59 daily position:100")), "23:59 daily position:100");
}

TEST(ScheduleText, RenderingNeverOverrunsTheBuffer) {
    // Eight entries with the longest possible day list, into a short buffer.
    const auto r = parse("00:00 mon,tue,wed,thu,fri,sat,sun position:100");
    ASSERT_TRUE(r.ok) << r.error;
    char small[16];
    std::memset(small, 0x7F, sizeof(small));
    format(r.entries, r.count, small, sizeof(small));
    EXPECT_EQ(small[sizeof(small) - 1], '\0')
        << "format() must always terminate within the buffer it was given";
}
