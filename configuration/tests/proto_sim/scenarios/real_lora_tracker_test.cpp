// The REAL lora_tracker.cpp, compiled and exercised.
//
// The point of this file is as much that it LINKS as what it asserts: until it
// existed, lora_tracker.cpp reached no compiler in this suite, and two review
// findings lived in that gap.

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

    EXPECT_FALSE(t.serviceTxQueue(2'999'999));
    EXPECT_TRUE(t.serviceTxQueue(3'000'000)) << "eligible at the instant, not after";
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 1);
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
