// The SHADOW pending-data mask: which bits the hub WOULD clear, never sent.
//
// The asymmetry that shapes every test here: a SET bit costs a node a 29 ms
// window, a CLEAR bit that was wrong costs a command up to 5.8 minutes of
// latency. So the tests lean on "never clears" — each reason to listen gets its
// own witness, and each is built from a fully QUIET baseline so the one reason
// under test is the only thing that can be holding the bit.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "PendingShadow.h"

using namespace pendingshadow;

namespace {

constexpr int64_t kInterval = (int64_t) timedgrid::kBeaconEveryRounds *
                              (int64_t) timedgrid::kRoundUs;   // ~349.5 s

// Every condition says "nothing for this node": the one state in which the
// bit may clear.
NodeInputs quiet(uint8_t slot) {
    NodeInputs n;
    n.slot                   = slot;
    n.op_awaiting_ack        = false;
    n.op_deferred_until_login = false;
    n.relogin_pending        = false;
    n.push_awaiting_ack      = false;
    n.timesync_due           = false;
    n.queued_frames          = false;
    n.timed_mode_off         = false;
    n.latency_tolerant       = true;
    n.since_last_traffic_us  = 10 * kInterval;
    return n;
}

bool bitSet(uint32_t mask, uint32_t slot) { return (mask & (1u << slot)) != 0; }

}  // namespace

TEST(PendingShadow, ADefaultNodeListensBecauseUnknownMeansListen) {
    // A zeroed NodeInputs is the "we know nothing" state, and it must not clear.
    NodeInputs n;
    const Verdict v = shadowMask(&n, 1, kInterval);
    EXPECT_FALSE(v.any_cleared);
    EXPECT_EQ(v.mask, pending::allListening());
}

TEST(PendingShadow, AFullyQuietNodeIsTheOneCaseThatClears) {
    const NodeInputs n = quiet(5);
    uint32_t reasons = 0xFFFFFFFFu;
    const Verdict v = shadowMask(&n, 1, kInterval, &reasons);
    EXPECT_EQ(reasons, 0u) << "precondition: the baseline has no reason to listen";
    EXPECT_TRUE(v.any_cleared);
    EXPECT_FALSE(bitSet(v.mask, 5));
    EXPECT_EQ(v.cleared_slots, 1u << 5);
    EXPECT_EQ(v.cleared_nodes, 1u);
    // Everything else stays set: only the one owned slot moved.
    EXPECT_EQ(v.mask, pending::allListening() & ~(1u << 5));
}

TEST(PendingShadow, EachReasonToListenHoldsTheBitOnItsOwn) {
    struct Case { const char *name; uint32_t reason; void (*apply)(NodeInputs &); };
    const Case cases[] = {
        {"op awaiting ack",        kOpAwaitingAck,        [](NodeInputs &n) { n.op_awaiting_ack = true; }},
        {"op deferred until login", kOpDeferredUntilLogin, [](NodeInputs &n) { n.op_deferred_until_login = true; }},
        {"relogin pending",        kReloginPending,       [](NodeInputs &n) { n.relogin_pending = true; }},
        {"push awaiting ack",      kPushAwaitingAck,      [](NodeInputs &n) { n.push_awaiting_ack = true; }},
        {"timesync due",           kTimeSyncDue,          [](NodeInputs &n) { n.timesync_due = true; }},
        {"frames queued",          kQueuedFrames,         [](NodeInputs &n) { n.queued_frames = true; }},
        {"timed mode off",         kTimedModeOff,         [](NodeInputs &n) { n.timed_mode_off = true; }},
        {"not latency tolerant",   kNotLatencyTolerant,   [](NodeInputs &n) { n.latency_tolerant = false; }},
        {"recent traffic",         kRecentTraffic,        [](NodeInputs &n) { n.since_last_traffic_us = kInterval; }},
    };
    for (const Case &c : cases) {
        NodeInputs n = quiet(9);
        c.apply(n);
        uint32_t reasons = 0;
        const Verdict v = shadowMask(&n, 1, kInterval, &reasons);
        EXPECT_EQ(reasons, c.reason) << c.name << ": exactly this reason, and no other";
        EXPECT_TRUE(bitSet(v.mask, 9)) << c.name << ": must hold the bit SET";
        EXPECT_FALSE(v.any_cleared) << c.name;
        EXPECT_EQ(v.mask, pending::allListening()) << c.name;
    }
}

TEST(PendingShadow, AStaleBitNeverClearsWhileAnOpIsPending) {
    // The named risk: a command is on its way to a node whose bit says "nothing
    // for you". Every flavour of "a command exists" holds the bit, whatever
    // else is quiet, however long the node has been silent.
    for (int which = 0; which < 3; ++which) {
        NodeInputs n = quiet(2);
        n.since_last_traffic_us = 1000 * kInterval;   // silent for ages
        if (which == 0) n.op_awaiting_ack = true;
        if (which == 1) n.op_deferred_until_login = true;
        if (which == 2) n.relogin_pending = true;
        const Verdict v = shadowMask(&n, 1, kInterval);
        EXPECT_TRUE(bitSet(v.mask, 2)) << "variant " << which;
        EXPECT_FALSE(v.any_cleared) << "variant " << which;
    }
}

TEST(PendingShadow, QuietMeansThreeWholeBeaconIntervalsAndNeverIsNotQuiet) {
    NodeInputs n = quiet(4);

    n.since_last_traffic_us = 3 * kInterval - 1;
    EXPECT_TRUE(bitSet(shadowMask(&n, 1, kInterval).mask, 4)) << "just inside 3 intervals";

    n.since_last_traffic_us = 3 * kInterval;
    EXPECT_FALSE(bitSet(shadowMask(&n, 1, kInterval).mask, 4)) << "3 intervals: quiet";

    n.since_last_traffic_us = -1;   // never heard, never addressed
    EXPECT_TRUE(bitSet(shadowMask(&n, 1, kInterval).mask, 4))
        << "a node with no history is unknown, not quiet";
    EXPECT_TRUE(recentTraffic(-1, kInterval));
    EXPECT_TRUE(recentTraffic(0, kInterval));
    EXPECT_FALSE(recentTraffic(3 * kInterval, kInterval));
}

TEST(PendingShadow, ASharedSlotIsTheOrOfItsNodes) {
    // login_slot_ % 32 wraps, so two nodes can own one slot and one bit.
    NodeInputs a = quiet(7), b = quiet(7);
    NodeInputs nodes[2] = {a, b};

    Verdict v = shadowMask(nodes, 2, kInterval);
    EXPECT_FALSE(bitSet(v.mask, 7)) << "both quiet: the shared bit clears";
    EXPECT_EQ(v.cleared_nodes, 0b11u);

    nodes[1].op_awaiting_ack = true;
    uint32_t reasons[2] = {0, 0};
    v = shadowMask(nodes, 2, kInterval, reasons);
    EXPECT_TRUE(bitSet(v.mask, 7)) << "one node busy: the shared bit stays SET";
    EXPECT_FALSE(v.any_cleared);
    EXPECT_EQ(v.cleared_nodes, 0u)
        << "the quiet one is NOT cleared: it shares a listening slot";
    EXPECT_EQ(reasons[0], 0u) << "...but its own reasons still say it was quiet";
    EXPECT_EQ(reasons[1], (uint32_t) kOpAwaitingAck);
}

TEST(PendingShadow, OnlyTheOwnedSlotMovesAndUnownedSlotsStayListening) {
    // A slot nobody on this hub owns may belong to a node this hub has not been
    // told about: unknown means listen.
    NodeInputs nodes[2] = {quiet(3), quiet(11)};
    nodes[1].timesync_due = true;
    const Verdict v = shadowMask(nodes, 2, kInterval);
    EXPECT_EQ(v.mask, pending::allListening() & ~(1u << 3));
    EXPECT_EQ(v.cleared_slots, 1u << 3);
    EXPECT_EQ(v.cleared_nodes, 0b01u);

    const Verdict none = shadowMask(nullptr, 0, kInterval);
    EXPECT_EQ(none.mask, pending::allListening()) << "no nodes: nothing cleared";
    EXPECT_FALSE(none.any_cleared);
}

TEST(PendingShadow, EverySlotWithAReasonStaysSetNotJustTheLastOne) {
    // Two different busy slots: both must hold, whichever order they arrive in.
    NodeInputs nodes[3] = {quiet(3), quiet(11), quiet(20)};
    nodes[0].op_awaiting_ack = true;
    nodes[1].op_awaiting_ack = true;
    const Verdict v = shadowMask(nodes, 3, kInterval);
    EXPECT_TRUE(bitSet(v.mask, 3));
    EXPECT_TRUE(bitSet(v.mask, 11));
    EXPECT_FALSE(bitSet(v.mask, 20)) << "the one quiet node's slot clears";
    EXPECT_EQ(v.cleared_nodes, 0b100u);
}

TEST(PendingShadow, AnOutOfRangeSlotOwnsNothing) {
    NodeInputs n = quiet((uint8_t) timedgrid::kSlotCount);   // one past the end
    const Verdict v = shadowMask(&n, 1, kInterval);
    EXPECT_EQ(v.mask, pending::allListening());
    EXPECT_FALSE(v.any_cleared);
}

TEST(PendingShadow, NodesBeyondThe32ndCanNeverClearAnything) {
    // Past kMaxNodes the verdict cannot name the node, so it refuses to clear
    // for it rather than clear unaccounted.
    NodeInputs nodes[kMaxNodes + 1];
    for (size_t i = 0; i < kMaxNodes + 1; ++i) nodes[i] = quiet(6);
    const Verdict v = shadowMask(nodes, kMaxNodes + 1, kInterval);
    EXPECT_TRUE(bitSet(v.mask, 6));
    EXPECT_FALSE(v.any_cleared);
}

TEST(PendingShadow, TheShadowMaskKeepsTheValidityFlagSemantics) {
    // The shadow verdict is a mask, nothing more: wrapped in the same Mask a
    // node would receive it follows PendingData.h's rules unchanged.
    NodeInputs n = quiet(5);
    const Verdict v = shadowMask(&n, 1, kInterval);

    pending::Mask valid;
    valid.bits = v.mask; valid.valid = true; valid.beacon_round = 100;
    valid.beacon_every_rounds = timedgrid::kBeaconEveryRounds;
    EXPECT_FALSE(pending::shouldArmWindow(valid, 5, 100)) << "its own cleared slot skips";
    EXPECT_TRUE(pending::shouldArmWindow(valid, 6, 100));

    pending::Mask unset;   // absent is not empty: the verdict does not change that
    unset.bits = v.mask;
    EXPECT_FALSE(unset.valid);
    EXPECT_TRUE(pending::shouldArmWindow(unset, 5, 100)) << "invalid mask: listen";
}

TEST(PendingShadow, DescribeReasonsNamesThemAndNeverOverflows) {
    char buf[64];
    describeReasons(0, buf, sizeof(buf));
    EXPECT_STREQ(buf, "none");
    describeReasons(kOpAwaitingAck | kPushAwaitingAck, buf, sizeof(buf));
    EXPECT_STREQ(buf, "op-ack,push");

    char tiny[8];
    describeReasons(0xFFFFFFFFu, tiny, sizeof(tiny));
    EXPECT_LT(std::string(tiny).size(), sizeof(tiny)) << "always NUL-terminated inside the buffer";
    describeReasons(1, nullptr, 0);   // must not crash
}

// Characterisation (CCN refactor): at EVERY buffer size the output is exactly
// the full string cut to cap-1 characters -- including the sizes where a name
// is split mid-way and where a comma is the last character that fits.
TEST(PendingShadow, DescribeReasonsIsTheFullStringTruncatedAtEveryCapacity) {
    const uint32_t all = kOpAwaitingAck | kOpDeferredUntilLogin | kReloginPending |
                         kPushAwaitingAck | kTimeSyncDue | kQueuedFrames |
                         kTimedModeOff | kNotLatencyTolerant | kRecentTraffic;
    const std::string full =
        "op-ack,op-deferred,relogin,push,timesync,queued,timed-off,not-tolerant,recent";
    char big[128];
    describeReasons(all, big, sizeof(big));
    ASSERT_EQ(std::string(big), full);
    for (size_t cap = 1; cap <= full.size() + 4; ++cap) {
        std::vector<char> buf(cap + 8, 'X');   // 8 guard bytes past the capacity
        describeReasons(all, buf.data(), cap);
        const std::string got(buf.data());
        EXPECT_EQ(got, full.substr(0, cap - 1)) << "cap=" << cap;
        for (size_t g = cap; g < buf.size(); ++g)
            EXPECT_EQ(buf[g], 'X') << "wrote past the capacity, cap=" << cap;
    }
    // A sparse set skips the bits it does not hold.
    describeReasons(kTimeSyncDue | kRecentTraffic, big, sizeof(big));
    EXPECT_STREQ(big, "timesync,recent");
    // "none" is also cut at the capacity.
    char three[3];
    describeReasons(0, three, sizeof(three));
    EXPECT_STREQ(three, "no");
}
