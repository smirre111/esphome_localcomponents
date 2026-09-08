// The pending-data bitmap — one bit per slot in the beacon, so a node with
// nothing waiting can skip its window.
//
// Both of the traps this header exists to avoid are silent and fleet-wide, so
// they get the hardest tests: a missing bitmap must not read as "nothing for
// anyone", and a permission to skip must expire even when the beacon that would
// have renewed it never arrives.
//
// See docs/implementation-plan.md section 4.4.

#include <gtest/gtest.h>

#include "PendingData.h"

using namespace pending;

namespace {
Mask valid(uint32_t bits, uint32_t round = 100, uint32_t every = 233) {
    Mask m;
    m.bits = bits;
    m.valid = true;
    m.beacon_round = round;
    m.beacon_every_rounds = every;
    return m;
}
}  // namespace

// ---------------------------------------------------------------------------
// Absent is not empty
// ---------------------------------------------------------------------------

TEST(PendingData, AZeroedMaskMeansListenNotSleep) {
    // The whole fleet's failure mode in one assertion. A proto3 zero is
    // indistinguishable from a deliberate "nothing for anyone", so a missing
    // bitmap must produce a listening node.
    Mask m;
    EXPECT_FALSE(m.valid);
    for (uint32_t slot = 0; slot < timedgrid::kSlotCount; ++slot)
        EXPECT_TRUE(shouldArmWindow(m, slot, 0)) << "slot " << slot;
}

TEST(PendingData, AValidAllClearMaskIsAllowedToSilenceEveryone) {
    // The legitimate version of the same shape: the hub really does have
    // nothing, and says so explicitly.
    const Mask m = valid(0x00000000);
    for (uint32_t slot = 0; slot < timedgrid::kSlotCount; ++slot)
        EXPECT_FALSE(shouldArmWindow(m, slot, m.beacon_round)) << "slot " << slot;
}

TEST(PendingData, ASetBitAlwaysArms) {
    const Mask m = valid(0b1010);
    EXPECT_TRUE(shouldArmWindow(m, 1, m.beacon_round));
    EXPECT_TRUE(shouldArmWindow(m, 3, m.beacon_round));
    EXPECT_FALSE(shouldArmWindow(m, 0, m.beacon_round));
    EXPECT_FALSE(shouldArmWindow(m, 2, m.beacon_round));
}

TEST(PendingData, AnOutOfRangeSlotListens) {
    // A node with no slot assignment, or one from a wider grid than this
    // firmware knows about. Neither is a reason to go deaf.
    const Mask m = valid(0x00000000);
    EXPECT_TRUE(shouldArmWindow(m, timedgrid::kSlotCount, m.beacon_round));
    EXPECT_TRUE(shouldArmWindow(m, 255, m.beacon_round));
    EXPECT_TRUE(hasDataFor(m, timedgrid::kSlotCount));
}

// ---------------------------------------------------------------------------
// A skip must expire
// ---------------------------------------------------------------------------

TEST(PendingData, ThePermissionExpiresWhenTheNextBeaconIsDue) {
    // Beacons are every 233 rounds by default — ~5.8 minutes. A node that
    // skipped until "the next beacon" and then missed it would be deaf for
    // nearly six minutes; one that kept trusting a stale mask would be deaf
    // indefinitely. The expiry is by ROUND NUMBER, so it happens whether or not
    // the beacon arrives.
    const Mask m = valid(0x00000000, /*round=*/100, /*every=*/233);
    EXPECT_EQ(expiresAtRound(m), 333u);

    EXPECT_FALSE(shouldArmWindow(m, 5, 100)) << "the beacon's own round";
    EXPECT_FALSE(shouldArmWindow(m, 5, 332)) << "the last round it covers";
    EXPECT_TRUE(shouldArmWindow(m, 5, 333))  << "the round the next beacon is due";
    EXPECT_TRUE(shouldArmWindow(m, 5, 5000)) << "and every round after it";
}

TEST(PendingData, AMaskWithNoCadenceExpiresImmediately) {
    // beaconEveryRounds == 0 means the hub is not beaconing. A mask that will
    // never be renewed may not authorise a single skipped window.
    const Mask m = valid(0x00000000, /*round=*/100, /*every=*/0);
    EXPECT_EQ(expiresAtRound(m), 100u);
    EXPECT_TRUE(shouldArmWindow(m, 5, 100));
    EXPECT_TRUE(shouldArmWindow(m, 5, 101));
}

TEST(PendingData, AClockThatMovedBackwardsListens) {
    // A round number before the beacon that carried the mask means the node's
    // idea of the grid changed — a resync, a re-adoption, a wraparound. The
    // mask describes a future the node is no longer in.
    const Mask m = valid(0x00000000, /*round=*/100);
    EXPECT_TRUE(shouldArmWindow(m, 5, 99));
    EXPECT_TRUE(shouldArmWindow(m, 5, 0));
}

TEST(PendingData, EveryReasonToDoubtProducesAListeningNode) {
    // The structural property, asserted as a property: across a sweep of
    // states, a node never skips unless the mask is valid, in date, and its bit
    // is genuinely clear. Written this way because the failure is silent — a
    // node that skips wrongly does not report anything, it just stops hearing.
    for (uint32_t round = 0; round < 600; round += 7) {
        for (uint32_t slot = 0; slot < timedgrid::kSlotCount; slot += 5) {
            for (bool is_valid : {false, true}) {
                for (uint32_t bits : {0x0u, 0xFFFFFFFFu, (1u << (slot % 32))}) {
                    Mask m = valid(bits, 100, 233);
                    m.valid = is_valid;
                    const bool skipped = !shouldArmWindow(m, slot, round);
                    if (skipped) {
                        EXPECT_TRUE(m.valid);
                        EXPECT_LT(round, expiresAtRound(m));
                        EXPECT_GE(round, m.beacon_round);
                        EXPECT_FALSE(hasDataFor(m, slot));
                    }
                }
            }
        }
    }
}

TEST(PendingData, TheSkipBudgetIsTheBeaconInterval) {
    const Mask m = valid(0x00000000, 100, 233);
    EXPECT_EQ(maxSkippableRounds(m, 5), 233u);
    EXPECT_EQ(maxSkippableRounds(m, 5) * timedgrid::kRoundUs / 1000000, 349u)
        << "349 s of skipped windows on one beacon — which is the saving, and "
           "also why the expiry has to be exact";

    const Mask has = valid(1u << 5, 100, 233);
    EXPECT_EQ(maxSkippableRounds(has, 5), 0u) << "this node has traffic waiting";

    Mask invalid;
    EXPECT_EQ(maxSkippableRounds(invalid, 5), 0u);
}

// ---------------------------------------------------------------------------
// The hub's side
// ---------------------------------------------------------------------------

TEST(PendingData, BitsAreSetAndClearedBySlot) {
    uint32_t bits = 0;
    bits = withSlot(bits, 0, true);
    bits = withSlot(bits, 31, true);
    EXPECT_EQ(bits, 0x80000001u);
    bits = withSlot(bits, 0, false);
    EXPECT_EQ(bits, 0x80000000u);
    // Out of range is ignored rather than wrapping onto slot 0.
    EXPECT_EQ(withSlot(bits, 32, true), bits);
    EXPECT_EQ(withSlot(bits, 99, true), bits);
}

TEST(PendingData, AHubThatCannotEnumerateItsQueuePublishesAllListening) {
    // The safe fallback, and it is NOT an empty mask. A hub that does not know
    // what it has pending must say "everyone listen", not "nobody bother".
    const uint32_t all = allListening();
    Mask m = valid(all);
    for (uint32_t slot = 0; slot < timedgrid::kSlotCount; ++slot)
        EXPECT_TRUE(shouldArmWindow(m, slot, m.beacon_round)) << "slot " << slot;
}
