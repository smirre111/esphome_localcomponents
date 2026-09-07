// Two nodes, two modes, one hub, one channel.
//
// Every other mode test looks at one mode in isolation. The deployment does
// not: a fleet migrates node by node, so for as long as the migration lasts a
// Mode A node (17-copy burst, listens on a periodic window) and a Mode B node
// (one copy, listens in its assigned slot) share the same hub and the same
// 433.3 MHz channel. This file asks whether they actually fit.
//
// The answer turns out to be governed by something the per-mode tests cannot
// see. Frames from one sender never COLLIDE — one radio transmits one frame at
// a time — but that is exactly why they conflict: the hub cannot begin a slot
// transmission while a burst copy is still going out. Hub air time is a single
// serial resource, and a 17-copy burst holds it for most of a round.
//
// See docs/test-plan.md section 4 and docs/mac-layer.md (MAC-0, access).

#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "sim/air_channel.h"

#include "LoraTiming.h"
#include "TimedGrid.h"
#include "ClassAWindows.h"
#include "TxQueue.h"

using namespace proto_sim;

namespace {

constexpr int kHub = 0;

// Payload sizes used throughout: a cover operation downlink and the ack the
// node sends back. Both are the sizes TimedGrid.h already reasons with, so the
// numbers here stay comparable with the slot-budget tests.
constexpr uint32_t kDownlinkLen = 60;
constexpr uint32_t kAckLen      = timedgrid::kAckPayloadBytes;

// The transmission the hub must make to serve a Mode B node in slot `k` of the
// round anchored at `anchor`.
Transmission slotFrame(int64_t anchor, uint32_t slot, uint32_t round = 0) {
    Transmission t;
    t.t0_us       = timedgrid::t0ForSlot(anchor, round, slot);
    t.payload_len = kDownlinkLen;
    t.tx_id       = kHub;
    t.seq         = slot;
    return t;
}

// Slots whose downlink the hub could not transmit, because a burst copy is
// still on the air at the moment that slot's frame would have to start.
std::set<uint32_t> slotsBlockedBy(const std::vector<Transmission>& burst_txs,
                                  int64_t anchor) {
    std::set<uint32_t> blocked;
    for (uint32_t k = 0; k < timedgrid::kSlotCount; ++k)
        if (anyAirOverlap(slotFrame(anchor, k), burst_txs))
            blocked.insert(k);
    return blocked;
}

}  // namespace

// ---------------------------------------------------------------------------
// The conflict
// ---------------------------------------------------------------------------

TEST(MixedMode, ABurstCopyIsShorterThanItsStrideButNotByMuch) {
    // The premise everything below rests on, stated once with its arithmetic
    // visible. If a copy occupied less than half its stride the conflict would
    // be mild; it does not.
    const int64_t occupancy = (int64_t) loratiming::kPreambleToT0Us +
                              (int64_t) loratiming::t0ToRxDoneUs(kDownlinkLen);
    EXPECT_LT(occupancy, (int64_t) loratiming::kBurstCopyStrideUs)
        << "a copy that outlasts its stride would make the burst self-overlapping";
    // Air is busy for roughly half of every stride.
    EXPECT_GT(occupancy * 100 / (int64_t) loratiming::kBurstCopyStrideUs, 40);
    EXPECT_LT(occupancy * 100 / (int64_t) loratiming::kBurstCopyStrideUs, 60);
}

TEST(MixedMode, AFullBurstDeniesAlmostTheEntireRound) {
    // The result that decides how mixed operation has to be scheduled.
    //
    // A copy occupies 42.0 ms of a 46.875 ms slot pitch — 90 % of a slot — and
    // the 88 ms stride is 1.878 pitches, so successive copies walk across the
    // slot boundaries rather than landing on them. The union covers 31 of the
    // 32 slots. Only the last one survives, and only because the 17th copy ends
    // before it.
    //
    // So a 17-copy burst and the timed grid do not interleave: for the round it
    // runs in, the grid is gone. Mixed operation therefore cannot mean "both at
    // once" — it means the hub must either serve the Mode A node with a single
    // copy (see ASingleCopyDownlinkBlocksAtMostOneSlot below) or spend a whole
    // round on the burst and none of the timed nodes.
    const int64_t anchor = 0;
    const auto burst_txs = burst(/*first_t0=*/0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    const auto blocked = slotsBlockedBy(burst_txs, anchor);

    EXPECT_EQ(blocked.size(), (size_t) 31);
    EXPECT_EQ(blocked.count(timedgrid::kSlotCount - 1), 0u)
        << "the final slot is the only survivor";
}

TEST(MixedMode, TheBlockedSetIsNotAnArtefactOfHowItIsComputed) {
    const int64_t anchor = 0;
    const auto burst_txs = burst(0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    const auto blocked = slotsBlockedBy(burst_txs, anchor);

    // 31 of 32 is a suspicious enough number to be worth checking against the
    // predicate directly, slot by slot, rather than trusting the set builder.
    for (uint32_t k = 0; k < timedgrid::kSlotCount; ++k) {
        const Transmission f = slotFrame(anchor, k);
        const bool is_blocked = blocked.count(k) != 0;
        EXPECT_EQ(is_blocked, anyAirOverlap(f, burst_txs)) << "slot " << k;
    }
}

TEST(MixedMode, ABlockedSlotFailsInTwoDifferentWays) {
    // What a Mode B node experiences when the hub could not serve its slot is
    // not one failure but two, and the funnel in mac-layer.md separates them
    // for exactly this reason:
    //
    //   * silence — nothing at all reaches the window. Nothing increments.
    //   * a foreign frame — a burst copy meant for the Mode A node happens to
    //     land inside the window. `detected` and `crcValid` both increment,
    //     `addressed` does not. The node burned a wake and a CRC on someone
    //     else's traffic.
    //
    // A KPI that stopped at `detected` would read the second case as a healthy
    // link. That is why the funnel does not stop there.
    const int64_t anchor = 0;
    const auto burst_txs = burst(0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    const auto blocked = slotsBlockedBy(burst_txs, anchor);
    ASSERT_FALSE(blocked.empty());

    size_t silent = 0, foreign = 0;
    for (uint32_t k : blocked) {
        const int64_t t0 = timedgrid::t0ForSlot(anchor, 0, k);
        const RxWindow w{timedgrid::windowOpenUs(t0), timedgrid::kWindowUs};
        bool any = false;
        for (const auto& t : burst_txs)
            if (caught(w, t, timedgrid::kDetectUs)) { any = true; break; }
        (any ? foreign : silent)++;
    }

    EXPECT_GT(silent, 0u)  << "some blocked slots simply hear nothing";
    EXPECT_GT(foreign, 0u) << "and some hear a frame addressed to another node";
    EXPECT_EQ(silent + foreign, blocked.size());
}

TEST(MixedMode, AFrameCaughtInTheWrongSlotIsStillTheWrongFrame) {
    // The address filter is what makes the second case above harmless, and it
    // is a MAC-1 concern, not a timing one: nothing about the window geometry
    // distinguishes a copy meant for this node from a copy meant for another.
    // Stated here so the mixed-mode story does not quietly assume otherwise.
    const int64_t anchor = 0;
    const uint32_t mode_b_slot = 0;
    const int64_t t0 = timedgrid::t0ForSlot(anchor, 0, mode_b_slot);
    const RxWindow w{timedgrid::windowOpenUs(t0), timedgrid::kWindowUs};

    // Copy 0 of a burst aimed at the Mode A node starts exactly at this T0.
    const auto burst_txs = burst(t0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    EXPECT_TRUE(caught(w, burst_txs[0], timedgrid::kDetectUs))
        << "geometry alone admits it";
    // ...and the hub could not have sent the Mode B node's own frame here.
    EXPECT_TRUE(anyAirOverlap(slotFrame(anchor, mode_b_slot), burst_txs));
}

// ---------------------------------------------------------------------------
// What makes the modes fit
// ---------------------------------------------------------------------------

TEST(MixedMode, ASingleCopyDownlinkBlocksAtMostOneSlot) {
    // B-1's per-frame copy count is not an optimisation here, it is the thing
    // that makes mixed operation possible at all: a Mode A node that is awake
    // and confirmed can be served with one copy, and one copy costs the grid
    // one slot instead of twenty-odd.
    const int64_t anchor = 0;
    for (uint32_t start_slot = 0; start_slot < timedgrid::kSlotCount; ++start_slot) {
        const auto single = burst(timedgrid::t0ForSlot(anchor, 0, start_slot),
                                  kDownlinkLen, /*copies=*/1,
                                  loratiming::kBurstCopyStrideUs, kHub);
        const auto blocked = slotsBlockedBy(single, anchor);
        EXPECT_EQ(blocked.size(), (size_t) 1) << "start slot " << start_slot;
        EXPECT_EQ(*blocked.begin(), start_slot);
    }
}

TEST(MixedMode, TheBeaconReservationIsFarTooSmallToParkABurstIn) {
    // TimedGrid::beaconClearSlots() reserves room for a hub broadcast, and it
    // is tempting to reach for that hole as the place a burst could live. It
    // cannot: the burst spans an order of magnitude more air than the beacon
    // reservation covers. There is no hole in the round big enough — which is
    // the same conclusion as the test above, arrived at from the grid's side.
    const uint32_t reserved = timedgrid::beaconClearSlots();
    EXPECT_GE(reserved, 1u);
    const int64_t burst_span =
        (int64_t) (loratiming::kBurstCopies - 1) * loratiming::kBurstCopyStrideUs;
    EXPECT_GT(burst_span, (int64_t) reserved * timedgrid::kSlotPitchUs);
    // Concretely: the burst outlasts the whole round's worth of slots it would
    // need to borrow.
    EXPECT_GT(burst_span * 100 / (int64_t) timedgrid::kRoundUs, 90);
}

// ---------------------------------------------------------------------------
// A third radio
// ---------------------------------------------------------------------------

TEST(MixedMode, AForeignTransmitterCollidesWhereTheHubMerelyBlocks) {
    // The distinction the two predicates draw, made explicit: a frame from a
    // DIFFERENT transmitter destroys the hub's frame (both are lost), while the
    // hub's own frames only prevent each other from starting.
    Transmission hub_frame = slotFrame(0, 4);

    Transmission foreign = hub_frame;
    foreign.tx_id = 99;
    foreign.t0_us += 1000;              // overlapping, not simultaneous

    EXPECT_TRUE(collides(hub_frame, foreign));
    EXPECT_TRUE(airOverlaps(hub_frame, foreign));

    Transmission own = hub_frame;
    own.t0_us += 1000;
    EXPECT_FALSE(collides(hub_frame, own)) << "one radio cannot jam itself";
    EXPECT_TRUE(airOverlaps(hub_frame, own)) << "but it cannot send both either";

    // And the channel model agrees: both frames are lost to the foreign one.
    const RxWindow w{timedgrid::windowOpenUs(hub_frame.t0_us), timedgrid::kWindowUs};
    const auto r = run({hub_frame, foreign}, {w});
    EXPECT_EQ(r.collided, 2u);
    EXPECT_EQ(r.windows_hit, 0u);
}

// ---------------------------------------------------------------------------
// Mode C alongside the others
// ---------------------------------------------------------------------------

TEST(MixedMode, AClassANodesRx1IsUnaffectedByTheGridButNotByTheBurst) {
    // Mode C needs no clock agreement with the hub — its windows hang off its
    // own uplink. That independence buys nothing against hub occupancy: if the
    // hub is mid-burst when RX1 opens, the reply cannot start.
    const int64_t t0_uplink = 500000;
    const int64_t rx1_open  = classa::rx1OpenUs(t0_uplink);

    // The reply the hub would have to send to land in RX1.
    Transmission reply;
    reply.t0_us       = t0_uplink + (int64_t) classa::kRx1DelayUs;
    reply.payload_len = kDownlinkLen;
    reply.tx_id       = kHub;

    const RxWindow rx1{rx1_open, timedgrid::kWindowUs};
    EXPECT_TRUE(caught(rx1, reply, timedgrid::kDetectUs))
        << "the reply is aimed at RX1 by construction";

    // Now put a burst on the air that straddles that instant.
    const auto burst_txs = burst(reply.t0_us - 40000, kDownlinkLen,
                                 loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    EXPECT_TRUE(anyAirOverlap(reply, burst_txs))
        << "hub occupancy defeats a window the node placed perfectly";
}

TEST(MixedMode, TheNodesUplinkAndTheHubsBurstCanCollide) {
    // Mode C transmits when it wants to. Unlike the hub's own frames, a node's
    // uplink is a foreign transmitter as far as a burst copy is concerned, so
    // this is a genuine collision and both frames are lost. It is the reason
    // Mode C is the LAST fallback and not the default.
    const auto burst_txs = burst(0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);

    Transmission uplink;
    uplink.t0_us       = burst_txs[3].t0_us + 5000;   // inside copy 3's air time
    uplink.payload_len = kAckLen;
    uplink.tx_id       = 1;

    EXPECT_TRUE(collides(uplink, burst_txs[3]));

    std::vector<Transmission> all = burst_txs;
    all.push_back(uplink);
    const auto r = run(all, {});
    EXPECT_EQ(r.collided, 2u) << "the uplink and exactly one copy are lost";
}

// ---------------------------------------------------------------------------
// The B3 gate, and the demotion counter it depends on
// ---------------------------------------------------------------------------

TEST(MixedMode, OneRoundClearsTheAirButTwoIsStillTheRule) {
    // The B3 gate of test-plan.md section 5.6 — no burst copy overlaps the
    // Mode B node's window while the hub also has a frame for that node — can
    // only hold by moving the slot frame out of the round the burst owns.
    //
    // How far out is where two separate constraints have to be kept apart, and
    // this test exists mostly to keep them apart. On the AIR, one round is
    // enough: the burst's last copy ends at 1.447 s, inside the 1.5 s round, so
    // every slot of round n+1 is already clear. The two-round rule of
    // section 4.5 does NOT come from air occupancy. It comes from the hub's
    // single serialised sendTask, which blocks a further ~400 ms after the
    // burst — 1850 ms against a 1500 ms round — and that is invisible to any
    // channel model.
    //
    // Stated as a test so nobody later "simplifies" kDeferRounds to 1 on the
    // strength of a geometry argument that does not reach the constraint.
    const int64_t anchor = 0;
    const auto burst_txs = burst(0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);

    for (uint32_t k = 0; k < timedgrid::kSlotCount; ++k) {
        EXPECT_FALSE(anyAirOverlap(slotFrame(anchor, k, /*round=*/1), burst_txs))
            << "slot " << k << ": the air is clear one round later";
        EXPECT_FALSE(anyAirOverlap(slotFrame(anchor, k, /*round=*/2), burst_txs))
            << "slot " << k;
    }

    // The margin is thin: the burst ends 53 ms before the round does, which is
    // barely more than one slot pitch. It is not a comfortable one-round rule
    // even on the air.
    const int64_t burst_air_end =
        (int64_t) (loratiming::kBurstCopies - 1) * loratiming::kBurstCopyStrideUs +
        (int64_t) loratiming::t0ToRxDoneUs(kDownlinkLen);
    EXPECT_LT(burst_air_end, (int64_t) timedgrid::kRoundUs);
    EXPECT_LT((int64_t) timedgrid::kRoundUs - burst_air_end,
              2 * (int64_t) timedgrid::kSlotPitchUs)
        << "less than two slots of headroom";

    // And the rule the queue actually implements is two rounds.
    EXPECT_EQ(txqueue::deferUntilUs(0, timedgrid::kRoundUs),
              2 * (int64_t) timedgrid::kRoundUs);
}

TEST(MixedMode, TheMissedMarkCounterMustKeyOnAddressedFramesNotOnSilence) {
    // The demotion trap. A node in Mode B counts consecutive marks with no
    // downlink and demotes itself when the count crosses a threshold. If that
    // counter keys on "nothing received", the traffic of the OTHER node in the
    // fleet resets it: a burst copy addressed elsewhere lands in the window,
    // the node concludes its link is alive, and it never demotes even though it
    // has been served nothing for as long as the bursts continue.
    //
    // Modelled here as the two counters the node could keep, over the 31 denied
    // slots. They must disagree, or the distinction the plan insists on would
    // be academic.
    const int64_t anchor = 0;
    const auto burst_txs = burst(0, kDownlinkLen, loratiming::kBurstCopies,
                                 loratiming::kBurstCopyStrideUs, kHub);
    const auto blocked = slotsBlockedBy(burst_txs, anchor);

    uint32_t marks_with_nothing_received = 0;   // the wrong counter
    uint32_t marks_with_nothing_for_me   = 0;   // the right one

    for (uint32_t k : blocked) {
        const int64_t t0 = timedgrid::t0ForSlot(anchor, 0, k);
        const RxWindow w{timedgrid::windowOpenUs(t0), timedgrid::kWindowUs};

        bool heard_anything = false;
        for (const auto& t : burst_txs)
            if (caught(w, t, timedgrid::kDetectUs)) { heard_anything = true; break; }

        // Nothing in this round was addressed to the Mode B node: the hub could
        // not transmit its frame at all.
        marks_with_nothing_for_me++;
        if (!heard_anything) marks_with_nothing_received++;
    }

    EXPECT_EQ(marks_with_nothing_for_me, (uint32_t) blocked.size());
    EXPECT_LT(marks_with_nothing_received, marks_with_nothing_for_me)
        << "the two counters must differ, or keying on one over the other is moot";
}
