// TimeSyncPolicy — Mode B and plain interactive nodes unthrottled by decision;
// an automatic-mode node's resync narrowed to once a week.

#include <gtest/gtest.h>

#include "TimeSyncPolicy.h"

using namespace timesyncpolicy;

TEST(TimeSyncPolicy, NonAutoModeIsAlwaysSentWhateverTheElapsedTime) {
    EXPECT_TRUE(shouldSend(/*auto_mode=*/false, /*elapsed=*/-1));
    EXPECT_TRUE(shouldSend(false, 0));
    EXPECT_TRUE(shouldSend(false, 1));
    EXPECT_TRUE(shouldSend(false, kAutoModeIntervalUs - 1));
    EXPECT_TRUE(shouldSend(false, kAutoModeIntervalUs));
}

TEST(TimeSyncPolicy, TheFirstSendForAnAutomaticNodeAlwaysGoesThrough) {
    // elapsed < 0 is the "never sent to this node" sentinel — shouldRunAutoMode()
    // cannot evaluate the schedule without an initial clock.
    EXPECT_TRUE(shouldSend(/*auto_mode=*/true, /*elapsed=*/-1));
}

TEST(TimeSyncPolicy, AnAutomaticNodeIsThrottledToOnceAWeek) {
    EXPECT_FALSE(shouldSend(true, 0));
    EXPECT_FALSE(shouldSend(true, kAutoModeIntervalUs - 1));
    EXPECT_TRUE(shouldSend(true, kAutoModeIntervalUs));
    EXPECT_TRUE(shouldSend(true, kAutoModeIntervalUs + 1));
}

TEST(TimeSyncPolicy, TheIntervalIsExactlyOneWeek) {
    EXPECT_EQ(kAutoModeIntervalUs, 7LL * 24 * 3600 * 1000000);
}
