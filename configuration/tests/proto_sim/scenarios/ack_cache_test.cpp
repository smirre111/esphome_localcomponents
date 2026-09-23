// AckCache — answering a replayed command instead of dropping it (B4).
//
// The bug it fixes is live: a lost ack today makes the hub retry, the node drop
// the retry as a replay, and the blind move twice while the hub reports
// failure. The trap it must avoid is worse: Mode A sends SEVENTEEN copies of
// every command, and a naive cached ack answers all sixteen duplicates.
//
// See docs/implementation-plan.md 4.7.

#include <gtest/gtest.h>

#include "AckCache.h"

using namespace ackcache;

namespace {
constexpr int64_t kT0 = 1000000;

Cache accepted(uint32_t msgid = 42, int64_t at = kT0) {
    Cache c;
    c.note(msgid, at);
    return c;
}
}  // namespace

// ---------------------------------------------------------------------------
// The burst-copy trap
// ---------------------------------------------------------------------------

TEST(AckCache, EverySubsequentCopyOfTheSameBurstIsSilent) {
    // Sixteen extra uplinks per command, on a battery node, for a command that
    // already succeeded, would be worse than the bug being fixed.
    const Cache c = accepted();
    for (int i = 1; i < 17; ++i) {
        const int64_t arrival = kT0 + (int64_t) i * loratiming::kBurstCopyStrideUs;
        EXPECT_EQ(classify(c, 42, arrival), Decision::SilentCopy)
            << "copy " << i;
    }
}

TEST(AckCache, TheBurstSpanCoversAWholeBurst) {
    // The discriminator only works if the span really does bracket all 17
    // copies and still falls well short of the 3 s retry interval.
    EXPECT_GT(kBurstSpanUs, 16 * (int64_t) loratiming::kBurstCopyStrideUs);
    EXPECT_LT(kBurstSpanUs, 3000000) << "a genuine retry must land outside it";
}

// ---------------------------------------------------------------------------
// The case it exists for
// ---------------------------------------------------------------------------

TEST(AckCache, ARetryAfterTheBurstIsAnswered) {
    // 3 s later: the hub's retry interval. The ack was lost, the command was
    // executed, and the node must say so rather than drop in silence.
    const Cache c = accepted();
    EXPECT_EQ(classify(c, 42, kT0 + 3000000), Decision::ReAck);
}

TEST(AckCache, ADifferentMsgidIsNotADuplicate) {
    const Cache c = accepted(42);
    EXPECT_EQ(classify(c, 43, kT0 + 3000000), Decision::NotADuplicate);
    EXPECT_EQ(classify(c, 41, kT0 + 3000000), Decision::NotADuplicate);
}

TEST(AckCache, AnEmptyCacheNeverClaimsADuplicate) {
    Cache c;
    EXPECT_EQ(classify(c, 42, kT0), Decision::NotADuplicate);
}

// ---------------------------------------------------------------------------
// Bounding the cost
// ---------------------------------------------------------------------------

TEST(AckCache, ReAcksAreRateLimited) {
    Cache c = accepted();
    ASSERT_EQ(classify(c, 42, kT0 + 3000000), Decision::ReAck);
    noteReAck(c, 42, kT0 + 3000000);

    // Immediately afterwards: suppressed, however many retries arrive.
    EXPECT_EQ(classify(c, 42, kT0 + 3100000), Decision::SilentCopy);
    EXPECT_EQ(classify(c, 42, kT0 + 4000000), Decision::SilentCopy);
    // Once the interval has passed: answered again.
    EXPECT_EQ(classify(c, 42, kT0 + 3000000 + kMinReAckIntervalUs),
              Decision::ReAck);
}

TEST(AckCache, ReAcksAreFinite) {
    // If the hub still has not heard us after kMaxReAcks, the problem is not a
    // lost ack and more uplinks will not fix it.
    Cache c = accepted();
    int64_t t = kT0 + 3000000;
    for (uint8_t i = 0; i < kMaxReAcks; ++i) {
        ASSERT_EQ(classify(c, 42, t), Decision::ReAck) << "re-ack " << (int) i;
        noteReAck(c, 42, t);
        t += kMinReAckIntervalUs;
    }
    EXPECT_EQ(classify(c, 42, t), Decision::Exhausted);
    EXPECT_EQ(classify(c, 42, t + 100000000), Decision::Exhausted)
        << "exhausted stays exhausted; it is not a timeout";
}

TEST(AckCache, ANewCommandClearsTheLedger) {
    Cache c = accepted(42);
    int64_t t = kT0 + 3000000;
    for (uint8_t i = 0; i < kMaxReAcks; ++i) { noteReAck(c, 42, t); t += kMinReAckIntervalUs; }
    ASSERT_EQ(classify(c, 42, t), Decision::Exhausted);

    c.note(43, t);
    EXPECT_EQ(classify(c, 43, t + 3000000), Decision::ReAck)
        << "the next command starts with a full budget";
    EXPECT_EQ(c.reacks(), 0u);
}

// ---------------------------------------------------------------------------
// Clock hygiene
// ---------------------------------------------------------------------------

TEST(AckCache, AClockThatJumpsBackwardsStaysSilent) {
    // Negative elapsed time must not be read as "long ago" and turn a burst
    // copy into a retry. Silence costs nothing; a spurious uplink costs
    // battery, so the conservative direction is the right one.
    const Cache c = accepted();
    EXPECT_EQ(classify(c, 42, kT0 - 5000000), Decision::SilentCopy);
}

TEST(AckCache, TheFirstReAckIsNotBlockedByAnUnsetTimestamp) {
    // last_reack_us starts at 0, which is a valid instant. Comparing against it
    // without the "has re-acked at all" guard would suppress the first re-ack
    // for the first 2 s of node uptime.
    Cache c = accepted(42, /*at=*/0);
    EXPECT_EQ(c.lastReackUs(), 0);
    EXPECT_EQ(classify(c, 42, 3000000), Decision::ReAck);
}

TEST(AckCache, ResetForgetsEverything) {
    Cache c = accepted();
    c.reset();
    EXPECT_FALSE(c.valid());
    EXPECT_EQ(classify(c, 42, kT0 + 3000000), Decision::NotADuplicate);
}

// ---------------------------------------------------------------------------
// Several commands at once
//
// The cache used to hold ONE, and the next acked command evicted it — so B4's
// whole purpose, making a lost ack recoverable, failed whenever any other acked
// command arrived in between. Found end to end: a ScheduleConfig ack evicted the
// cover op the hub was about to retry.
// ---------------------------------------------------------------------------

TEST(AckCache, AnotherAckedCommandDoesNotEvictTheOneStillBeingRetried) {
    Cache c;
    c.note(42, kT0);
    c.note(43, kT0);          // e.g. a schedule push, acked right after

    EXPECT_EQ(classify(c, 42, kT0 + 3000000), Decision::ReAck)
        << "the first command must still be re-ackable, or its lost ack is "
           "unrecoverable and the hub tears down a working session";
    EXPECT_EQ(classify(c, 43, kT0 + 3000000), Decision::ReAck);
}

TEST(AckCache, EachCommandGetsItsOwnReAckBudget) {
    // The budget bounds how many times ONE command may be answered again. Shared
    // across commands it would be spent by whichever arrived first.
    Cache c;
    c.note(42, kT0);
    c.note(43, kT0);

    int64_t t = kT0 + 3000000;
    for (uint8_t i = 0; i < kMaxReAcks; ++i) { noteReAck(c, 42, t); t += kMinReAckIntervalUs; }
    EXPECT_EQ(classify(c, 42, t), Decision::Exhausted);
    EXPECT_EQ(classify(c, 43, t), Decision::ReAck) << "43 has spent nothing";
}

TEST(AckCache, ReNotingAnIdDoesNotMintFreshBudget) {
    // Otherwise a replay refreshes the very budget that bounds how often it can
    // be answered — an attacker could keep a node transmitting indefinitely.
    Cache c;
    c.note(42, kT0);
    int64_t t = kT0 + 3000000;
    for (uint8_t i = 0; i < kMaxReAcks; ++i) { noteReAck(c, 42, t); t += kMinReAckIntervalUs; }
    ASSERT_EQ(classify(c, 42, t), Decision::Exhausted);

    c.note(42, t);
    EXPECT_EQ(classify(c, 42, t + 3000000), Decision::Exhausted)
        << "re-noting must not reset first_seen_us or the re-ack count";
}

TEST(AckCache, TheOldestEntryIsTheOneEvicted) {
    // Four entries cover every frame the hub can have in flight to one node.
    // Past that, the command whose retry ladder ran out longest ago is the one
    // to forget.
    Cache c;
    for (uint32_t id = 1; id <= Cache::kEntries; ++id) c.note(id, kT0);
    c.note(99, kT0);          // one too many

    EXPECT_EQ(classify(c, 1, kT0 + 3000000), Decision::NotADuplicate)
        << "the oldest is gone";
    EXPECT_EQ(classify(c, 2, kT0 + 3000000), Decision::ReAck);
    EXPECT_EQ(classify(c, 99, kT0 + 3000000), Decision::ReAck);
}
