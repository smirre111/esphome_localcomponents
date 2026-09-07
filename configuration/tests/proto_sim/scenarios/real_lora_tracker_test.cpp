// The REAL lora_tracker.cpp, compiled and exercised.
//
// The point of this file is as much that it LINKS as what it asserts: until it
// existed, lora_tracker.cpp reached no compiler in this suite, and two review
// findings lived in that gap.

#include <gtest/gtest.h>

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "lora_hal_recorder.h"
#include "TimedGrid.h"
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
