// The REAL lora_tracker.cpp, compiled and exercised.
//
// The point of this file is as much that it LINKS as what it asserts: until it
// existed, lora_tracker.cpp reached no compiler in this suite, and two review
// findings lived in that gap.

#include "PendingData.h"
#include <gtest/gtest.h>

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "lora_hal_recorder.h"
#include "TimedGrid.h"
#include "LoraTiming.h"
#include "esp_timer.h"
#include "blinds.pb-c.h"

#include <cstring>
#include <vector>

using esphome::lora_tracker::LORATracker;
using esphome::lora_tracker::TxPolicy;

namespace {

// A real, packable operation frame. The burst loop only re-stamps copies it can
// unpack; a buffer of zeroes fails to unpack, so a test that fed one would take
// the raw-bytes fallback and never touch the indexing code at all.
std::vector<uint8_t> packedOperationFrame() {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 7;
    hdr.senderaddress = 1;
    hdr.msgid         = 42;

    LoraCoverOperation op = LORA_COVER_OPERATION__INIT;

    LoraClientOperationMessage msg = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    msg.header    = &hdr;
    msg.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
    msg.operation = &op;

    std::vector<uint8_t> buf(lora_client_operation_message__get_packed_size(&msg));
    lora_client_operation_message__pack(&msg, buf.data());
    return buf;
}

} // namespace

TEST(RealTracker, TheGridAnchorIsSetOnceAndNeverMoves) {
    LORATracker t;
    EXPECT_FALSE(t.gridStarted());
    t.startGrid();
    ASSERT_TRUE(t.gridStarted());
    const int64_t a = t.gridAnchorUs();
    t.startGrid();
    EXPECT_EQ(t.gridAnchorUs(), a);
}

TEST(RealTracker, SlotsArePitchApartOnTheRealImplementation) {
    // The shim mirrors this arithmetic; here it is the production copy.
    LORATracker t;
    t.startGrid();
    const int64_t base = t.gridAnchorUs();
    for (uint8_t k = 0; k + 1 < timedgrid::kSlotCount; ++k)
        EXPECT_EQ(t.nextT0ForSlotUs(k + 1, base) - t.nextT0ForSlotUs(k, base),
                  (int64_t) timedgrid::kSlotPitchUs) << "slot " << (int) k;
}

TEST(RealTracker, NextT0IsNeverInThePast) {
    LORATracker t;
    t.startGrid();
    const int64_t a = t.gridAnchorUs();
    for (int64_t off = 0; off < (int64_t) timedgrid::kRoundUs; off += 13000)
        EXPECT_GE(t.nextT0ForSlotUs(11, a + off), a + off) << "off " << off;
}

TEST(RealTracker, MsUntilNextT0IsZeroWithoutAGrid) {
    LORATracker t;
    ASSERT_FALSE(t.gridStarted());
    EXPECT_EQ(t.msUntilNextT0(5), 0u) << "no grid means send now, not wait";
}

TEST(RealTracker, ABurstEmitsTheDefaultCopyCount) {
    // sendPacketBurst is called DIRECTLY: sendTask is an infinite loop and is
    // not run here. This is the first time the production burst loop has been
    // executed by any test.
    lorahal::rec().reset();
    LORATracker t;
    uint8_t frame[24] = {0};
    t.sendPacketBurst(frame, sizeof(frame));

    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 17)
        << "the default burst is txSlotsPerRound copies (lora_tx is the fire)";
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 17);
}

TEST(RealTracker, APerFrameCopyCountIsHonouredByTheRealBurstLoop) {
    // B-1's whole point, on the production code path rather than the shim.
    lorahal::rec().reset();
    LORATracker t;
    uint8_t frame[24] = {0};
    t.sendPacketBurst(frame, sizeof(frame), /*copies=*/1, /*stride_ms=*/0);

    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 1)
        << "one copy means one frame on the air";
}

TEST(RealTracker, EachCopyIsStampedWithItsOwnBurstIndex) {
    // The indexing contract the node depends on to know when a burst ends:
    // every copy carries the same burstCount and its own 0-based burstIndex.
    lorahal::rec().reset();
    LORATracker t;
    auto frame = packedOperationFrame();
    t.sendPacketBurst(frame.data(), frame.size(), /*copies=*/5, /*stride_ms=*/0);

    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 5);
    for (size_t i = 0; i < lorahal::rec().packets.size(); ++i) {
        const auto& p = lorahal::rec().packets[i];
        LoraClientOperationMessage* m =
            lora_client_operation_message__unpack(NULL, p.size(), p.data());
        ASSERT_NE(m, nullptr) << "copy " << i << " did not unpack";
        ASSERT_NE(m->header, nullptr);
        EXPECT_EQ(m->header->burstcount, 5u) << "copy " << i;
        EXPECT_EQ(m->header->burstindex, (uint32_t) i);
        EXPECT_EQ(m->header->msgid, 42u) << "re-stamping must not disturb the msgid";
        lora_client_operation_message__free_unpacked(m, NULL);
    }
}

TEST(RealTracker, ASingleCopyStillCarriesABurstCountOfOne) {
    // burstCount == 0 is the wire's "not part of a burst" marker. A one-copy
    // burst must not accidentally claim that: the node would skip the deferral
    // it still needs for the frame it is about to answer.
    lorahal::rec().reset();
    LORATracker t;
    auto frame = packedOperationFrame();
    t.sendPacketBurst(frame.data(), frame.size(), /*copies=*/1, /*stride_ms=*/0);

    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    const auto& p = lorahal::rec().packets.front();
    LoraClientOperationMessage* m =
        lora_client_operation_message__unpack(NULL, p.size(), p.data());
    ASSERT_NE(m, nullptr);
    ASSERT_NE(m->header, nullptr);
    EXPECT_EQ(m->header->burstcount, 1u);
    EXPECT_EQ(m->header->burstindex, 0u);
    lora_client_operation_message__free_unpacked(m, NULL);
}

TEST(RealTracker, AnUnparseableFrameIsStillSentVerbatim) {
    // The OOM / non-protobuf fallback. It matters that this path exists: the
    // tracker also carries raw bytes (OTA chunks), and dropping them because
    // they are not an operation message would be silent data loss.
    lorahal::rec().reset();
    LORATracker t;
    uint8_t raw[24];
    for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = (uint8_t) (0xA0 + i);
    t.sendPacketBurst(raw, sizeof(raw), /*copies=*/3, /*stride_ms=*/0);

    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 3);
    for (const auto& p : lorahal::rec().packets)
        EXPECT_EQ(p, std::vector<uint8_t>(raw, raw + sizeof(raw)));
}

// ---------------------------------------------------------------------------
// The 1850 ms serialisation, read off the production defaults
// ---------------------------------------------------------------------------

namespace {
// responseWindowMs is protected. It is also the single number that makes the
// two-round deferral rule non-negotiable (test-plan.md section 4.2, and
// mixed_mode_test.cpp::OneRoundClearsTheAirButTwoIsStillTheRule, which shows
// the AIR is clear after one round — so this is the only thing left holding
// the rule up). Reading it through a derived class beats copying the literal
// into a test, where it would stop tracking the source.
struct TrackerProbe : LORATracker {
    using LORATracker::responseWindowMs;
    using LORATracker::txIntervalMs;
    using LORATracker::txSlotsPerRound;
};
}  // namespace

TEST(RealTracker, SendTaskBlocksAFurther400msAfterABurst) {
    TrackerProbe p;
    EXPECT_EQ(p.responseWindowMs, 400);
    // txIntervalMs is `roundDurationMs / txSlotsPerRound` — 1500/17 — and it
    // lands on 88 only because C++ truncates. The exact quotient, 88.235, is
    // the -2663 ppm ruler that cost three firmware revisions. The truncation is
    // load-bearing; assert it rather than trusting it.
    EXPECT_EQ(p.txIntervalMs, 88);
    EXPECT_EQ(p.txIntervalMs * 1000u, loratiming::kBurstCopyStrideUs)
        << "the hub's stride must equal the node's compiled kCopySpacingUs";
    EXPECT_EQ(p.txSlotsPerRound, 17);

    // 16 strides + one frame's air time + the response window.
    const int64_t burst_us =
        (int64_t) (p.txSlotsPerRound - 1) * p.txIntervalMs * 1000 +
        (int64_t) loratiming::t0ToRxDoneUs(60) + (int64_t) loratiming::kPreambleToT0Us;
    const int64_t occupied_us = burst_us + (int64_t) p.responseWindowMs * 1000;

    EXPECT_LT(burst_us, (int64_t) timedgrid::kRoundUs)
        << "the burst itself fits in a round";
    EXPECT_GT(occupied_us, (int64_t) timedgrid::kRoundUs)
        << "but the task does not: this is why deferral is two rounds, not one";
    EXPECT_NEAR((double) occupied_us, 1850000.0, 5000.0);
}

// ---------------------------------------------------------------------------
// Bx — the hub learns when a packet arrived
// ---------------------------------------------------------------------------

namespace {
// checkReception() is public; the poll loop that drives it is not run here, so
// tests call it directly and move the harness clock between calls, which is
// exactly what a poll gap is.
struct RxProbe : LORATracker {
    void poll() { this->checkReception(); }
};

std::vector<uint8_t> frameOf(size_t n) { return std::vector<uint8_t>(n, 0x5A); }
}  // namespace

TEST(RealTrackerRx, NoPacketLeavesTheTimestampUnset) {
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(1'000'000);

    RxProbe t;
    t.poll();
    t.poll();

    EXPECT_EQ(t.last_rx_done_us(), 0) << "an empty poll must not stamp anything";
    EXPECT_EQ(t.last_rx_t0_us(), 0);
}

TEST(RealTrackerRx, T0IsRxDoneMinusThePayloadsSymbols) {
    // The whole point of the timestamp: T0, the SFD end, which is the single
    // reference both ends agree on. Never decomposed by hand.
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(5'000'000);

    RxProbe t;
    t.poll();                                    // establishes the poll baseline
    proto_sim_timer_advance_us(10'000);          // one loop() iteration
    lorahal::rec().queueRx(frameOf(60));
    t.poll();

    const int64_t rxdone = t.last_rx_done_us();
    EXPECT_EQ(t.last_rx_t0_us(),
              loratiming::t0FromRxDoneUs(rxdone, 60));
    EXPECT_LT(t.last_rx_t0_us(), rxdone) << "T0 precedes RxDone by the payload";
}

TEST(RealTrackerRx, TheEstimateIsTheMidpointOfThePollGap) {
    // RxDone happened somewhere in (previous poll, this poll]. Stamping "now"
    // would be biased late by a whole gap; the midpoint is unbiased, and the
    // uncertainty is half the gap. Both are reported, neither assumed.
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(2'000'000);

    RxProbe t;
    t.poll();
    const int64_t prev_poll = 2'000'000;
    proto_sim_timer_advance_us(12'000);
    const int64_t this_poll = prev_poll + 12'000;

    lorahal::rec().queueRx(frameOf(45));
    t.poll();

    EXPECT_EQ(t.last_rx_done_us(), this_poll - 6'000);
    EXPECT_EQ(t.rx_stamp_uncertainty_us(), 6'000u);
    EXPECT_GT(t.last_rx_done_us(), prev_poll) << "not before the previous poll";
    EXPECT_LT(t.last_rx_done_us(), this_poll) << "not after this one";
}

TEST(RealTrackerRx, TheUncertaintyTracksTheMeasuredGapNotTheNominalDelay) {
    // loop() delays 10 ms; the GAP is that plus whatever else the main loop
    // did. Assuming 10 ms would understate the error exactly when the hub is
    // busy, which is when it matters.
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(0);

    RxProbe t;
    t.poll();
    proto_sim_timer_advance_us(47'000);          // a slow iteration
    lorahal::rec().queueRx(frameOf(60));
    t.poll();

    EXPECT_EQ(t.rx_stamp_uncertainty_us(), 23'500u);
    EXPECT_EQ(t.worst_poll_gap_us(), 47'000u);
}

TEST(RealTrackerRx, TheWorstGapIsAHighWaterMark) {
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(0);

    RxProbe t;
    t.poll();
    proto_sim_timer_advance_us(30'000);
    t.poll();
    proto_sim_timer_advance_us(9'000);
    t.poll();

    EXPECT_EQ(t.worst_poll_gap_us(), 30'000u)
        << "a later quiet iteration must not erase the outlier";
}

TEST(RealTrackerRx, ThePollPathCannotMeetC2sGate) {
    // Stated as a test so the claim is checked rather than remembered. C2 wants
    // T0 to ±1 ms; a 10 ms poll gives ±5 ms at best, and that is the NOMINAL
    // gap. Closing it needs DIO0 wired to the ESP32 and an ISR stamp — a
    // hardware change, since DIO0 is not in the hub's pin map at all.
    lorahal::rec().reset();
    proto_sim_timer_set_now_us(0);

    RxProbe t;
    t.poll();
    proto_sim_timer_advance_us(10'000);          // the nominal loop() delay
    lorahal::rec().queueRx(frameOf(60));
    t.poll();

    EXPECT_GT(t.rx_stamp_uncertainty_us(), 1'000u)
        << "if this ever passes, the poll path got faster than C2's gate and "
           "the ISR work can be reconsidered";
    EXPECT_EQ(t.rx_stamp_uncertainty_us(), 5'000u);
}

// ---------------------------------------------------------------------------
// B1a — the reordering, time-scheduled transmit queue
// ---------------------------------------------------------------------------

namespace {
// sendTask() is an infinite loop with blocking waits, so the scheduling step
// was split out of it. This is the whole reason serviceTxQueue() exists as a
// method: the interesting behaviour — which frame goes next, and when — is
// testable, and the task around it is a thin loop.
struct TxProbe : LORATracker {
    using LORATracker::serviceTxQueue;
    using LORATracker::nextTxEligibleUs;
    void init() { this->init_memory_pool(); }
};

// A frame the burst loop will send as raw bytes, with a recognisable first byte
// so the order of transmission can be read off the recorder.
std::vector<uint8_t> tagged(uint8_t tag) {
    std::vector<uint8_t> v(24, tag);
    return v;
}

uint8_t firstByteOfPacket(size_t i) {
    return lorahal::rec().packets.at(i).front();
}
}  // namespace

TEST(RealTrackerTx, AnEligibleFrameIsSentAndAnIdleQueueReportsNothingToDo) {
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    EXPECT_FALSE(t.serviceTxQueue(0)) << "nothing queued, nothing sent";
    EXPECT_EQ(t.nextTxEligibleUs(0), INT64_MAX) << "and nothing to wait for";

    auto f = tagged(0xA1);
    t.send(f.data(), f.size(), TxPolicy{/*copies=*/1});
    EXPECT_TRUE(t.serviceTxQueue(0));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(firstByteOfPacket(0), 0xA1);
}

TEST(RealTrackerTx, ADeferredFrameIsHeldAndTheWaitIsBounded) {
    // The property a FIFO cannot provide. Before this, sendTask blocked on
    // xQueueReceive(portMAX_DELAY), so a deferred frame waited not until it was
    // eligible but until some UNRELATED traffic happened to wake the task.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    auto f = tagged(0xB2);
    TxPolicy later;
    later.copies      = 1;
    later.earliest_us = 3'000'000;          // two rounds out
    t.send(f.data(), f.size(), later);

    EXPECT_FALSE(t.serviceTxQueue(0)) << "not yet";
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 0);
    EXPECT_EQ(t.nextTxEligibleUs(0), 3'000'000)
        << "and the task knows exactly how long to sleep";

    // Released kPrepareLeadUs EARLY, and not one microsecond earlier than that.
    // The lead is what firePacket then busy-waits out: a frame handed over only
    // once its instant had passed could never be fired ON it, because the whole
    // prepare — idle, preamble, FIFO clock-in — would still be ahead of it.
    const int64_t lead = LORATracker::kPrepareLeadUs;
    EXPECT_FALSE(t.serviceTxQueue(3'000'000 - lead - 1));
    EXPECT_TRUE(t.serviceTxQueue(3'000'000 - lead))
        << "eligible one prepare-lead before the instant, not after it";
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 1);
}

TEST(RealTrackerTx, AMiscomputedInstantCannotSpinTheRadioMutexForever) {
    // kMaxFireBusyWaitUs is documented as "the ceiling for a MISCOMPUTED
    // instant", but the cap was applied to `remaining` and then `remaining` was
    // recomputed from not_before_us inside the loop — so the ceiling was
    // restored to its original value on every iteration and the loop spun all
    // the way to not_before_us. preparePacket() takes the radio mutex and only
    // firePacket() gives it, so an uncapped spin holds the mutex that the main
    // ESPHome loop needs in checkReception(): the watchdog fires before
    // anything logs a problem.
    //
    // Today popDue only releases a frame within kPrepareLeadUs of its instant,
    // so this is reached only by a caller that computes its own instant — which
    // is precisely what B5's prepare/fire split was built to allow.
    lorahal::rec().reset();
    proto_sim_timer_reset();
    TxProbe t;
    t.init();

    constexpr int64_t kStart = 1'000'000;
    proto_sim_timer_set_now_us(kStart);

    // An instant a full minute out: a caller that computed against the wrong
    // clock, which is the failure the ceiling exists for.
    constexpr int64_t kAbsurd = kStart + 60'000'000;

    auto f = tagged(0xC0);
    ASSERT_TRUE(t.preparePacket(f.data(), f.size()));
    ASSERT_TRUE(t.firePacket(kAbsurd));

    ASSERT_EQ(lorahal::rec().tx_us.size(), (size_t) 1);
    const int64_t waited = lorahal::rec().tx_us[0] - kStart;
    EXPECT_LE(waited, LORATracker::kMaxFireBusyWaitUs)
        << "the busy wait must be bounded by the ceiling, not by the instant "
           "the caller got wrong";
    EXPECT_LT(lorahal::rec().tx_us[0], kAbsurd)
        << "and it must not have spun all the way to the bad instant";
}

TEST(RealTrackerTx, APlacedFrameFiresAtItsInstantNotWhenItIsPopped) {
    // B5's gate, end to end. preparePacket/firePacket were built, tested and
    // then called from exactly one site with not_before_us hardcoded to 0, so
    // the split existed and placed nothing: every frame still fired whenever
    // the queue got round to it, carrying the full prepare cost — idle,
    // preamble, FIFO clock-in — in front of the one register write that is the
    // actual fire instant.
    lorahal::rec().reset();
    proto_sim_timer_reset();
    TxProbe t;
    t.init();

    constexpr int64_t kTarget = 3'000'000;
    const int64_t     lead    = LORATracker::kPrepareLeadUs;

    auto f = tagged(0xB5);
    TxPolicy placed;
    placed.copies      = 1;      // a placed frame is one copy; a burst is the
    placed.stride_ms   = 0;      // opposite construction
    placed.earliest_us = kTarget;
    t.send(f.data(), f.size(), placed);

    // The task wakes one prepare-lead early and services the queue there. The
    // harness clock has to agree with the instant handed in, because firePacket
    // busy-waits against esp_timer_get_time() and delayMicroseconds advances it.
    proto_sim_timer_set_now_us(kTarget - lead);
    ASSERT_TRUE(t.serviceTxQueue(kTarget - lead));

    ASSERT_EQ(lorahal::rec().tx_us.size(), (size_t) 1);
    EXPECT_EQ(lorahal::rec().tx_us[0], kTarget)
        << "the frame must leave at the instant it was placed for, not at the "
           "instant the scheduler happened to release it";
}

TEST(RealTrackerTx, AnEligibleFrameOvertakesADeferredOneAheadOfIt) {
    // "Not before round n+2, and behind nothing else" — the sentence section
    // 4.5 says the old API could not express.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    auto held = tagged(0xC1);
    TxPolicy defer; defer.copies = 1; defer.earliest_us = 3'000'000;
    t.send(held.data(), held.size(), defer);

    auto now = tagged(0xC2);
    t.send(now.data(), now.size(), TxPolicy{/*copies=*/1});

    ASSERT_TRUE(t.serviceTxQueue(0));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(firstByteOfPacket(0), 0xC2) << "the second frame went first";

    ASSERT_TRUE(t.serviceTxQueue(3'000'000));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 2);
    EXPECT_EQ(firstByteOfPacket(1), 0xC1);
}

TEST(RealTrackerTx, EqualPriorityKeepsInsertionOrder) {
    // The tiebreak is insertion order, never earliest_us: two frames both
    // eligible now must not be reordered by a field that no longer constrains
    // either of them.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    auto a = tagged(0xD1);
    TxPolicy early; early.copies = 1; early.earliest_us = 0;
    t.send(a.data(), a.size(), early);

    auto b = tagged(0xD2);
    TxPolicy earlier; earlier.copies = 1; earlier.earliest_us = -1000;
    t.send(b.data(), b.size(), earlier);

    ASSERT_TRUE(t.serviceTxQueue(10'000));
    ASSERT_TRUE(t.serviceTxQueue(10'000));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 2);
    EXPECT_EQ(firstByteOfPacket(0), 0xD1);
    EXPECT_EQ(firstByteOfPacket(1), 0xD2);
}

TEST(RealTrackerTx, AnImmediateFrameJumpsAnEligibleNormalOne) {
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    auto normal = tagged(0xE1);
    t.send(normal.data(), normal.size(), TxPolicy{/*copies=*/1});

    auto urgent = tagged(0xE2);
    TxPolicy now; now.copies = 1; now.priority = 0;   // Immediate
    t.send(urgent.data(), urgent.size(), now);

    ASSERT_TRUE(t.serviceTxQueue(0));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(firstByteOfPacket(0), 0xE2);
}

TEST(RealTrackerTx, PriorityDoesNotOverrideTheDeferral) {
    // An Immediate frame that is not yet eligible must still wait. Otherwise
    // "immediate" would silently mean "ignore the channel reservation", which
    // is exactly the collision the deferral exists to avoid.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    auto urgent = tagged(0xF1);
    TxPolicy p; p.copies = 1; p.priority = 0; p.earliest_us = 3'000'000;
    t.send(urgent.data(), urgent.size(), p);

    EXPECT_FALSE(t.serviceTxQueue(0));
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 0);
    EXPECT_TRUE(t.serviceTxQueue(3'000'000));
}

TEST(RealTrackerTx, TheDefaultPolicyIsSendNowAtNormalPriority) {
    // Every existing caller passes TxPolicy{} or nothing. If the defaults
    // changed behaviour, B1a would be a silent regression across the hub.
    TxPolicy p;
    EXPECT_EQ(p.earliest_us, 0);
    EXPECT_EQ(p.priority, 1);

    lorahal::rec().reset();
    TxProbe t;
    t.init();
    auto f = tagged(0x11);
    t.send(f.data(), f.size(), TxPolicy{/*copies=*/1});
    EXPECT_TRUE(t.serviceTxQueue(0)) << "eligible immediately";
}

// ---------------------------------------------------------------------------
// B5 — PREPARE and FIRE as separate acts
// ---------------------------------------------------------------------------

namespace {
struct FireProbe : LORATracker {
    using LORATracker::preparePacket;
    using LORATracker::firePacket;
    using LORATracker::abortPreparedPacket;
};

// Index of the first occurrence of a call, or SIZE_MAX.
size_t indexOf(const char* fn) {
    const auto& c = lorahal::rec().calls;
    for (size_t i = 0; i < c.size(); ++i) if (c[i] == fn) return i;
    return SIZE_MAX;
}
}  // namespace

TEST(RealTrackerFire, PrepareLoadsTheFifoAndDoesNotTransmit) {
    // The whole point: after PREPARE the frame is in the radio and nothing has
    // gone out. Everything whose duration varies with payload length — the
    // FIFO clocking above all — is behind us.
    lorahal::rec().reset();
    FireProbe t;
    uint8_t frame[60];
    for (size_t i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t) i;

    t.preparePacket(frame, sizeof(frame));

    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 0)
        << "PREPARE must not fire";
    EXPECT_EQ(lorahal::rec().count("lora_beginPacket"), (size_t) 1);
    EXPECT_EQ(lorahal::rec().staging.size(), (size_t) 60)
        << "the payload is in the FIFO, waiting";

    t.firePacket(0);
    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 1);
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(lorahal::rec().packets[0].size(), (size_t) 60);
}

TEST(RealTrackerFire, NothingLengthDependentSitsBetweenPrepareAndTheFire) {
    // Stated as an ordering assertion because that is what the gate is about.
    // If a payload write, a config write or a mode change ever reappears after
    // PREPARE, the fire instant inherits its variance again.
    lorahal::rec().reset();
    FireProbe t;
    uint8_t frame[152];
    memset(frame, 0x77, sizeof(frame));

    t.preparePacket(frame, sizeof(frame));
    const size_t calls_after_prepare = lorahal::rec().calls.size();
    t.firePacket(0);

    // Between PREPARE returning and lora_tx there must be nothing at all.
    const size_t tx_at = indexOf("lora_tx");
    ASSERT_NE(tx_at, SIZE_MAX);
    EXPECT_EQ(tx_at, calls_after_prepare)
        << "the fire is the very next thing the radio is asked to do";
}

TEST(RealTrackerFire, FireWithoutPrepareIsRefusedRatherThanSendingStaleBytes) {
    lorahal::rec().reset();
    FireProbe t;
    EXPECT_FALSE(t.firePacket(0));
    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 0)
        << "whatever is in the FIFO is not ours";
}

TEST(RealTrackerFire, ASecondPrepareDiscardsTheFirstInsteadOfSplicingIt) {
    // Two PREPAREs with no FIRE between them would otherwise clock the second
    // payload in behind the first and send them as one frame.
    lorahal::rec().reset();
    FireProbe t;
    uint8_t a[20]; memset(a, 0xA0, sizeof(a));
    uint8_t b[30]; memset(b, 0xB0, sizeof(b));

    t.preparePacket(a, sizeof(a));
    t.preparePacket(b, sizeof(b));
    t.firePacket(0);

    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(lorahal::rec().packets[0].size(), (size_t) 30)
        << "only the second frame goes out, whole";
    EXPECT_EQ(lorahal::rec().packets[0].front(), 0xB0);
}

TEST(RealTrackerFire, AbortLeavesTheRadioListeningNotHalfArmed) {
    lorahal::rec().reset();
    FireProbe t;
    uint8_t frame[24]; memset(frame, 0x33, sizeof(frame));

    t.preparePacket(frame, sizeof(frame));
    t.abortPreparedPacket();

    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 0);
    EXPECT_GE(lorahal::rec().count("lora_receive"), (size_t) 1)
        << "a radio left armed for a transmit that never comes is deaf";
    EXPECT_FALSE(t.firePacket(0)) << "and there is nothing left to fire";
}

TEST(RealTrackerFire, SendPacketBytesStillDoesBothAndIsUnchanged) {
    // Every existing caller goes through sendPacketBytes. The split must not
    // alter what it does.
    lorahal::rec().reset();
    FireProbe t;
    uint8_t frame[45]; memset(frame, 0x5C, sizeof(frame));

    t.sendPacketBytes(frame, sizeof(frame));

    EXPECT_EQ(lorahal::rec().count("lora_beginPacket"), (size_t) 1);
    EXPECT_EQ(lorahal::rec().count("lora_tx"), (size_t) 1);
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(lorahal::rec().packets[0].size(), (size_t) 45);
}

// ---------------------------------------------------------------------------
// Section 4.5's slot-aware deferral, on the production arithmetic
// ---------------------------------------------------------------------------

namespace {
struct DeferProbe : TxProbe {
    using LORATracker::busyUntilUs;
    using LORATracker::nextClearT0ForSlotUs;
};
}  // namespace

TEST(RealTrackerDefer, WithNoBurstInFlightAClearMarkIsJustTheNextMark) {
    DeferProbe t;
    t.startGrid();
    const int64_t a = t.gridAnchorUs();
    EXPECT_EQ(t.busyUntilUs(), 0);
    for (uint8_t slot : {0, 5, 17, 31})
        EXPECT_EQ(t.nextClearT0ForSlotUs(slot, a),
                  t.nextT0ForSlotUs(slot, a)) << "slot " << (int) slot;
}

TEST(RealTrackerDefer, ABurstPushesAMarkOutByAWholeRound) {
    // The measured result behind this: a 17-copy burst denies 31 of the 32
    // slots in its round, so there is no hole to slide into — the only correct
    // answer is the same slot, a round later.
    lorahal::rec().reset();
    DeferProbe t;
    t.init();
    t.startGrid();
    const int64_t a = t.gridAnchorUs();

    // Send a default (full) burst, which sets the busy window.
    auto frame = packedOperationFrame();
    proto_sim_timer_set_now_us(a);
    t.send(frame.data(), frame.size(), TxPolicy{});
    ASSERT_TRUE(t.serviceTxQueue(a));

    const int64_t busy = t.busyUntilUs();
    EXPECT_GT(busy, a + (int64_t) timedgrid::kRoundUs)
        << "burst plus the response window outlasts a round — which is why the "
           "deferral is two rounds and not one";

    // Every slot's next clear mark must be at or after the channel is free.
    for (uint8_t slot = 0; slot < timedgrid::kSlotCount; ++slot) {
        const int64_t clear = t.nextClearT0ForSlotUs(slot, a);
        EXPECT_GE(clear, busy) << "slot " << (int) slot;
        // ...and it must still be one of THIS slot's marks, not a neighbour's.
        EXPECT_EQ((clear - t.nextT0ForSlotUs(slot, a)) % (int64_t) timedgrid::kRoundUs, 0)
            << "slot " << (int) slot << " was moved off its own grid";
    }
}

TEST(RealTrackerDefer, TheBusyWindowIsDeclaredBeforeTheBurstNotAfterIt) {
    // A caller deciding where to place a timed downlink must see the burst that
    // is ABOUT to run. If the window were set afterwards, a frame queued during
    // the burst would be placed against a channel the hub already knows is
    // occupied — and land in it.
    lorahal::rec().reset();
    DeferProbe t;
    t.init();
    t.startGrid();

    proto_sim_timer_set_now_us(0);
    auto frame = packedOperationFrame();
    t.send(frame.data(), frame.size(), TxPolicy{});

    EXPECT_EQ(t.busyUntilUs(), 0) << "nothing sent yet";
    ASSERT_TRUE(t.serviceTxQueue(0));
    EXPECT_GT(t.busyUntilUs(), 0) << "and now the channel is spoken for";
}

TEST(RealTrackerDefer, ASingleCopyDownlinkBarelyMovesTheWindow) {
    // B4's remaining half, seen from the other side: one copy occupies the
    // channel for one frame plus the response window, not for a round and a
    // half. That is what makes serving a Mode A node cheap enough to interleave.
    lorahal::rec().reset();
    DeferProbe t;
    t.init();
    t.startGrid();

    proto_sim_timer_set_now_us(0);
    auto frame = packedOperationFrame();
    TxPolicy one; one.copies = 1;
    t.send(frame.data(), frame.size(), one);
    ASSERT_TRUE(t.serviceTxQueue(0));

    EXPECT_LT(t.busyUntilUs(), (int64_t) timedgrid::kRoundUs)
        << "a single copy must not deny the whole round";
    EXPECT_GT(t.busyUntilUs(), 0);
}

TEST(RealTrackerDefer, WithoutAGridThereIsNothingToDeferTo) {
    DeferProbe t;
    ASSERT_FALSE(t.gridStarted());
    EXPECT_EQ(t.msUntilNextClearT0(3), 0u)
        << "no grid means send now, the same answer msUntilNextT0 gives";
}

// ---------------------------------------------------------------------------
// Section 4.4 — the periodic broadcast beacon, on the real tracker.
//
// It lives here rather than on a listener because there is one grid per radio
// and one beacon for the whole fleet: a per-listener beacon would be 32
// broadcasts of the same frame, which is the unicast keepalive section 4.4
// prices at 3.4x worse than plain burst and rejects.
//
// What it BUYS is phase. Without it a node holds phase only for resyncMaxS
// after each addressed frame — at 3.5 commands/day that is 2.9 % of the day on
// the grid, and no measurable battery saving, so Mode B would be a narrower
// window for nothing.
// ---------------------------------------------------------------------------

TEST(RealTrackerBeacon, NoGridMeansNoBeacon) {
    proto_sim_timer_reset();
    TxProbe t;
    t.init();
    ASSERT_FALSE(t.gridStarted());
    t.serviceBeacon();
    EXPECT_EQ(t.beaconsSent(), 0u) << "there is no grid to beacon on";
}

TEST(RealTrackerBeacon, ABeaconIsQueuedOnceAndPlacedOnItsOwnMark) {
    lorahal::rec().reset();
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(1'000'000);
    TxProbe t;
    t.init();
    t.startGrid();

    const int64_t beacon_t0 = t.nextBeaconT0Us(esp_timer_get_time());
    ASSERT_GT(beacon_t0, 0);

    // Round 0 is a beacon round, so a hub that has just started its grid
    // beacons within its first round rather than waiting out a whole cadence:
    // the nodes it is about to admit need a mark to hold phase against.
    //
    // Queued once, however many times the loop asks.
    t.serviceBeacon();
    t.serviceBeacon();
    t.serviceBeacon();
    ASSERT_EQ(t.beaconsSent(), 1u) << "one beacon per beacon round";

    // Fire it, and read back what actually went on the air.
    const int64_t fire = loratiming::fireInstantUs(beacon_t0, 0);
    proto_sim_timer_set_now_us(fire - LORATracker::kPrepareLeadUs);
    ASSERT_TRUE(t.serviceTxQueue(fire - LORATracker::kPrepareLeadUs));

    ASSERT_EQ(lorahal::rec().tx_us.size(), (size_t) 1)
        << "one broadcast, on one mark — a burst is the opposite construction";
    EXPECT_EQ(lorahal::rec().tx_us[0], fire)
        << "a beacon that slips its mark is a beacon nobody is listening for";

    const auto &bytes = lorahal::rec().packets.at(0);
    LoraClientOperationMessage *msg = lora_client_operation_message__unpack(
        nullptr, bytes.size(), bytes.data());
    ASSERT_NE(msg, nullptr);
    ASSERT_EQ(msg->cmd_case, LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON);
    ASSERT_NE(msg->gridbeacon, nullptr);
    ASSERT_NE(msg->header, nullptr);

    EXPECT_EQ(msg->header->destaddress, (uint32_t) LORATracker::broadcastAddressing)
        << "one frame for 32 nodes at 32 different phases";
    // THE ROUND IT ACTUALLY GOES OUT IN. Declaring 0 would leave the node
    // numbering rounds from wherever the frame landed, and "beacon round" would
    // mean something different at each end.
    EXPECT_EQ(msg->gridbeacon->txround, t.beaconRoundForT0(beacon_t0));
    EXPECT_EQ(msg->gridbeacon->txround % timedgrid::kBeaconEveryRounds, 0u);
    EXPECT_EQ(msg->gridbeacon->txslot, timedgrid::kBeaconSlotIndex);

    // ALL LISTENING, and not a placeholder: a clear bit is still a promise this
    // hub cannot keep for an interactive node. Authenticating the beacon does
    // not by itself start clearing bits — those are two separate decisions.
    EXPECT_TRUE(msg->gridbeacon->pendingmaskvalid);
    EXPECT_EQ(msg->gridbeacon->pendingmask, pending::allListening());

    // Section 4.4: the beacon is SIGNED. An unsigned beacon from a hub that
    // holds a key is indistinguishable on the air from a forgery, so
    // serviceBeacon sends none at all rather than one without a tag.
    EXPECT_EQ(msg->gridbeacon->netkeyid, t.netKeyId());
    EXPECT_NE(msg->gridbeacon->netkeyid, 0u);
    ASSERT_EQ(msg->gridbeacon->mac.len, framecrypto::kBeaconMacBytes);

    // And it verifies under the hub's own key. Recomputed here from the LAYOUT
    // rather than compared against a stored tag: what can silently disagree
    // between the two ends is which bytes go into the MAC, and a golden tag
    // would pin the hub's arithmetic against the hub's own choice.
    uint8_t expect[framecrypto::kBeaconMacBytes];
    ASSERT_TRUE(t.beaconMac(msg->gridbeacon->txround, msg->gridbeacon->txslot,
                            msg->gridbeacon->pendingmask,
                            msg->gridbeacon->pendingmaskvalid,
                            expect, sizeof(expect)));
    EXPECT_EQ(memcmp(expect, msg->gridbeacon->mac.data, sizeof(expect)), 0);

    // The frame still fits the slot geometry it was priced against. A beacon
    // that outgrew kBeaconPayloadBytes would need a second clear slot after it
    // — beaconClearSlots() — and would blind a node on every beacon round
    // forever, which is a failure that would present as "node b+1 is
    // unreliable" and never be attributed to the beacon.
    EXPECT_LE(bytes.size(), (size_t) timedgrid::kBeaconPayloadBytes)
        << "the signed beacon must still fit the size the geometry assumes";

    lora_client_operation_message__free_unpacked(msg, nullptr);
}

TEST(RealTrackerBeacon, ABeaconIsNotQueuedMinutesAheadOfItsMark) {
    // The pool is five buffers deep and a placed frame holds one until its
    // instant, so queueing the next beacon as soon as the last one fired would
    // spend a fifth of the pool for 5.8 minutes.
    lorahal::rec().reset();
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(1'000'000);
    TxProbe t;
    t.init();
    t.startGrid();

    t.serviceBeacon();                       // round 0's beacon
    ASSERT_EQ(t.beaconsSent(), 1u);
    const int64_t first = t.nextBeaconT0Us(esp_timer_get_time());

    // Just past the first mark: the next one is a whole cadence out.
    proto_sim_timer_set_now_us(first + 1);
    t.serviceBeacon();
    EXPECT_EQ(t.beaconsSent(), 1u) << "nothing to queue yet";

    // And within a round of the next mark, it is queued.
    const int64_t second = t.nextBeaconT0Us(first + 1);
    proto_sim_timer_set_now_us(second - (int64_t) timedgrid::kRoundUs / 2);
    t.serviceBeacon();
    EXPECT_EQ(t.beaconsSent(), 2u);
}

TEST(RealTrackerBeacon, TheNextBeaconIsAWholeCadenceAfterTheLast) {
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(1'000'000);
    TxProbe t;
    t.init();
    t.startGrid();

    const int64_t first  = t.nextBeaconT0Us(esp_timer_get_time());
    const int64_t second = t.nextBeaconT0Us(first + 1);
    EXPECT_EQ(second - first,
              (int64_t) timedgrid::kRoundUs * (int64_t) timedgrid::kBeaconEveryRounds)
        << "the cadence is what bounds how long a node may hold phase";
    EXPECT_EQ(t.beaconRoundForT0(second) - t.beaconRoundForT0(first),
              timedgrid::kBeaconEveryRounds);
}

// ---------------------------------------------------------------------------
// Supersession — one Home Assistant gesture, one command on the air
//
// Placement is monotone per node, so a second command arriving before the first
// has fired is placed at the FOLLOWING mark — both are kept, deliberately, and
// that is right for two commands a person actually asked for.
//
// It was wrong for a superseded one. The hub tracks exactly ONE command per
// node: begin_tracked_op_ overwrites op_first_msgid_, so the moment the second
// arrives the hub will neither accept the first's ack nor retry it. Sending it
// anyway made the blind move to the first position and then, 1.5 s later, to
// the second — and the ack for the one it executed was logged as rejected.
// ---------------------------------------------------------------------------

TEST(RealTrackerTx, ASupersededFrameIsDroppedAtTheFrontOfTheQueue) {
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    TxPolicy first;
    first.copies        = 1;
    first.earliest_us   = 1'000'000;
    first.supersede_key = 18;          // the node's short address
    first.supersede_gen = 1;
    auto a = tagged(0xC1);
    ASSERT_TRUE(t.send(a.data(), a.size(), first));

    // The second gesture, before the first has fired. Placed a round later, as
    // the monotone rule requires — it is not a replacement in the queue.
    TxPolicy second = first;
    second.earliest_us   = 2'500'000;
    second.supersede_gen = 2;
    auto b = tagged(0xC2);
    ASSERT_TRUE(t.send(b.data(), b.size(), second));

    const int64_t lead = LORATracker::kPrepareLeadUs;

    // The first frame's mark arrives. It is no longer wanted, so nothing goes
    // out — and serviceTxQueue reports no work rather than pretending it did
    // some, because the next real frame must not be paced a service interval
    // behind a frame that was never sent.
    EXPECT_FALSE(t.serviceTxQueue(1'000'000 - lead));
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 0)
        << "the superseded command must never reach the air";
    EXPECT_EQ(t.supersededDrops(), 1u);

    // The second one does.
    EXPECT_TRUE(t.serviceTxQueue(2'500'000 - lead));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(firstByteOfPacket(0), 0xC2);
    EXPECT_EQ(t.supersededDrops(), 1u) << "and it is not dropped itself";
}

TEST(RealTrackerTx, ADropDoesNotStallTheFrameBehindIt) {
    // Both marks in the same service call. Popping one entry and returning as
    // if work had been done would hold the live frame until the next service
    // interval — which for a placed command is the round it was aimed at, so
    // the fix for a double move would have become a late one.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    TxPolicy p;
    p.copies        = 1;
    p.earliest_us   = 1'000'000;
    p.supersede_key = 18;
    p.supersede_gen = 1;
    auto a = tagged(0xD1);
    ASSERT_TRUE(t.send(a.data(), a.size(), p));

    p.supersede_gen = 2;
    auto b = tagged(0xD2);
    ASSERT_TRUE(t.send(b.data(), b.size(), p));   // same instant, both eligible

    const int64_t lead = LORATracker::kPrepareLeadUs;
    EXPECT_TRUE(t.serviceTxQueue(1'000'000 - lead))
        << "the live frame goes out in the SAME call that skipped the stale one";
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 1);
    EXPECT_EQ(firstByteOfPacket(0), 0xD2);
    EXPECT_EQ(t.supersededDrops(), 1u);
}

TEST(RealTrackerTx, ABeaconIsNotRetiredByACommand) {
    // Everything that is not a tracked op carries key 0. A beacon retired by
    // one node's command would be a broadcast the whole fleet stopped hearing
    // because somebody moved a blind.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    TxPolicy beacon;
    beacon.copies      = 1;
    beacon.earliest_us = 1'000'000;      // key 0 by default
    auto bc = tagged(0xE1);
    ASSERT_TRUE(t.send(bc.data(), bc.size(), beacon));

    TxPolicy cmd;
    cmd.copies        = 1;
    cmd.earliest_us   = 1'000'000;
    cmd.supersede_key = 18;
    cmd.supersede_gen = 99;
    auto c = tagged(0xE2);
    ASSERT_TRUE(t.send(c.data(), c.size(), cmd));

    const int64_t lead = LORATracker::kPrepareLeadUs;
    EXPECT_TRUE(t.serviceTxQueue(1'000'000 - lead));
    EXPECT_TRUE(t.serviceTxQueue(1'000'000 - lead));
    ASSERT_EQ(lorahal::rec().packets.size(), (size_t) 2);
    EXPECT_EQ(firstByteOfPacket(0), 0xE1) << "FIFO within a priority, unchanged";
    EXPECT_EQ(firstByteOfPacket(1), 0xE2);
    EXPECT_EQ(t.supersededDrops(), 0u);
}

TEST(RealTrackerTx, TheBufferOfADroppedFrameGoesBackToThePool) {
    // The pool is five buffers deep and a placed frame holds one until its
    // mark. A drop that leaked would exhaust it after five superseded commands
    // and every later downlink would be REFUSED — a worse failure than the one
    // being fixed, and one that would present as a dead radio.
    //
    // Three rounds of "queue four, drain": if the dropped buffers were not
    // returned, the pool would be empty by round two and send() would fail.
    lorahal::rec().reset();
    TxProbe t;
    t.init();

    const int64_t lead = LORATracker::kPrepareLeadUs;
    uint32_t gen = 0;
    for (int round = 0; round < 3; ++round)
    {
        const int64_t mark = 1'000'000 + (int64_t) round * 1'000'000;
        for (int i = 0; i < 4; ++i)
        {
            TxPolicy p;
            p.copies        = 1;
            p.earliest_us   = mark;
            p.supersede_key = 18;
            p.supersede_gen = ++gen;
            auto f = tagged((uint8_t) gen);
            ASSERT_TRUE(t.send(f.data(), f.size(), p))
                << "pool exhausted in round " << round << " — a dropped buffer leaked";
        }
        while (t.serviceTxQueue(mark - lead)) { }
    }

    // Three per round survive as drops, one per round goes out.
    EXPECT_EQ(t.supersededDrops(), 9u);
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 3);
    EXPECT_EQ(firstByteOfPacket(0), 4);
    EXPECT_EQ(firstByteOfPacket(1), 8);
    EXPECT_EQ(firstByteOfPacket(2), 12);
}
