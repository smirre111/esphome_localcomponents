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

#include <vector>

using esphome::lora_tracker::LORATracker;

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

    EXPECT_EQ(lorahal::rec().count("lora_endPacket"), (size_t) 17)
        << "the default burst is txSlotsPerRound copies";
    EXPECT_EQ(lorahal::rec().packets.size(), (size_t) 17);
}

TEST(RealTracker, APerFrameCopyCountIsHonouredByTheRealBurstLoop) {
    // B-1's whole point, on the production code path rather than the shim.
    lorahal::rec().reset();
    LORATracker t;
    uint8_t frame[24] = {0};
    t.sendPacketBurst(frame, sizeof(frame), /*copies=*/1, /*stride_ms=*/0);

    EXPECT_EQ(lorahal::rec().count("lora_endPacket"), (size_t) 1)
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
