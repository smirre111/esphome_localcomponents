// The hub's sleep model (SleepModel.h): the predicate that decides whether a
// command to an interactive node is ignored, its boot margin, the wall-clock
// rendering and the text of the "Sleep Status" sensor. Integration with the
// listener is pinned by SleepGate / SleepStatus in real_lora_client_test.cpp.

#include <gtest/gtest.h>

#include <cstring>

#include "SleepModel.h"

using namespace sleepmodel;

namespace {
// An acknowledged sleep at t=1000 s, not heard since, wake 5 h away, margin 11 s.
Inputs asleepInputs() {
    Inputs in{};
    in.auto_mode         = false;
    in.sleep_unconfirmed = false;
    in.last_sleep_epoch  = 1000;
    in.last_heard_epoch  = 999;
    in.ms_until_awake    = 18'000'000;
    in.margin_ms         = 11'000;
    return in;
}
}  // namespace

TEST(SleepModel, AnAcknowledgedSleepNotHeardSinceAndWellBeforeTheWakeIsAsleep) {
    EXPECT_EQ(classify(asleepInputs()), State::Asleep);
}

TEST(SleepModel, AnAutoModeNodeIsNeverModelledAsAsleep) {
    Inputs in = asleepInputs();
    in.auto_mode = true;
    EXPECT_EQ(classify(in), State::Auto);
}

TEST(SleepModel, AnUnconfirmedSleepIsTreatedAsAwake) {
    Inputs in = asleepInputs();
    in.sleep_unconfirmed = true;
    EXPECT_EQ(classify(in), State::Unconfirmed);
}

TEST(SleepModel, NoAcknowledgedSleepOnRecordMeansAwake) {
    Inputs in = asleepInputs();
    in.last_sleep_epoch = 0;
    in.last_heard_epoch = 0;
    EXPECT_EQ(classify(in), State::Awake);
    in.last_heard_epoch = 500;   // heard, never slept by the hub
    EXPECT_EQ(classify(in), State::Awake);
}

TEST(SleepModel, HearingTheNodeAtOrAfterTheSleepMakesItAwake) {
    Inputs in = asleepInputs();
    in.last_heard_epoch = 1000;
    EXPECT_EQ(classify(in), State::Awake) << "heard in the same second as the stamp";
    in.last_heard_epoch = 5000;
    EXPECT_EQ(classify(in), State::Awake);
}

TEST(SleepModel, TheNodeIsAwakeFromExactlyTheMarginBeforeTheWake) {
    Inputs in = asleepInputs();
    in.ms_until_awake = 11'001;
    EXPECT_EQ(classify(in), State::Asleep) << "one ms outside the margin";
    in.ms_until_awake = 11'000;
    EXPECT_EQ(classify(in), State::Awake) << "exactly the margin";
    in.ms_until_awake = 0;
    EXPECT_EQ(classify(in), State::Awake);
}

TEST(SleepModel, TheMarginIsTheBootMarginPlusTheLoginStaggerPerSlot) {
    EXPECT_EQ(kBootMarginMs, 5000u);
    EXPECT_EQ(kLoginStaggerMs, 3000u);
    EXPECT_EQ(marginMs(0), 5000u);
    EXPECT_EQ(marginMs(3), 14000u);
}

TEST(SleepModel, TheRefreshLandsJustPastTheMomentTheMarginOpens) {
    EXPECT_EQ(refreshDelayMs(100'000, 11'000), 90'000u);
    EXPECT_EQ(refreshDelayMs(11'000, 11'000), 1000u);
    EXPECT_EQ(refreshDelayMs(5'000, 11'000), 1000u) << "already inside: check back in a second";
}

TEST(SleepModel, ClockHmRendersLocalTimeOfDayIncludingAcrossMidnightAndNegativeOffsets) {
    unsigned h = 99, m = 99;
    clockHm(0, 0, &h, &m);
    EXPECT_EQ(h, 0u); EXPECT_EQ(m, 0u);
    clockHm(5 * 3600 + 7 * 60 + 59, 0, &h, &m);
    EXPECT_EQ(h, 5u); EXPECT_EQ(m, 7u);
    clockHm(23 * 3600 + 30 * 60, 3600, &h, &m);
    EXPECT_EQ(h, 0u); EXPECT_EQ(m, 30u) << "past midnight in local time";
    clockHm(3600, -7200, &h, &m);
    EXPECT_EQ(h, 23u); EXPECT_EQ(m, 0u) << "before the epoch's midnight: wraps, never negative";
}

TEST(SleepModel, TheStatusTextOfEachState) {
    char buf[40];
    statusText(State::Awake, 0, 0, buf, sizeof buf);
    EXPECT_STREQ(buf, "awake");
    statusText(State::Asleep, 5 * 3600, 0, buf, sizeof buf);
    EXPECT_STREQ(buf, "asleep until 05:00");
    statusText(State::Asleep, 5 * 3600, 7200, buf, sizeof buf);
    EXPECT_STREQ(buf, "asleep until 07:00") << "local time, not UTC";
    statusText(State::Unconfirmed, 0, 0, buf, sizeof buf);
    EXPECT_STREQ(buf, "sleep unconfirmed");
    statusText(State::Auto, 0, 0, buf, sizeof buf);
    EXPECT_STREQ(buf, "auto mode");
}
