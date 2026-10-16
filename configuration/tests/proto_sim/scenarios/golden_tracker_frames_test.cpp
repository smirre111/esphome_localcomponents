// S0 golden frames, tracker side (docs/mac-separation-layering-plan.md).
//
// Byte-exact FIFO contents written by the REAL lora_tracker.cpp: the signed
// GridBeacon it builds itself, and the per-copy re-stamp (burstIndex/Count,
// onMark, fire round/offset) of a plaintext frame in sendPacketBurst. The
// encrypted per-copy retag is pinned on the listener side
// (golden_frames_test.cpp), because this target links a seal stub.
//
// Inputs pinned: rand() seed (the shim's esp_random mints the fleet key and its
// id), the esp_timer clock, the frame handed in. Judged one process per test.

#include <gtest/gtest.h>

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "lora_hal_recorder.h"
#include "LoraTiming.h"
#include "esp_timer.h"
#include "blinds.pb-c.h"
#include "sim/golden.h"

#include <cstdlib>
#include <sstream>
#include <vector>

using esphome::lora_tracker::LORATracker;

namespace {

constexpr unsigned kSeed = 0x5EED0001u;

struct GProbe : LORATracker {
    using LORATracker::serviceTxQueue;
    void init() { this->init_memory_pool(); }
};

std::vector<uint8_t> plaintextOperation() {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 18;
    hdr.destsubnet    = 2;
    hdr.senderaddress = 1;
    hdr.msgid         = 42;
    LoraCoverOperation op = LORA_COVER_OPERATION__INIT;
    op.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
    op.operation  = COV_OPERATION__CMD_OPEN;
    LoraClientOperationMessage msg = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    msg.header    = &hdr;
    msg.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
    msg.operation = &op;
    std::vector<uint8_t> buf(lora_client_operation_message__get_packed_size(&msg));
    lora_client_operation_message__pack(&msg, buf.data());
    return buf;
}

std::string dumpRecorder() {
    std::ostringstream s;
    const auto &r = lorahal::rec();
    for (size_t i = 0; i < r.packets.size(); ++i)
        s << "fifo " << i << " tx_us=" << r.tx_us[i] << " " << golden::hex(r.packets[i]) << "\n";
    return s.str();
}

void pin() {
    std::srand(kSeed);
    lorahal::rec().reset();
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(1'000'000);
}

}  // namespace

TEST(GoldenTrackerFrames, SignedGridBeacon) {
    pin();
    GProbe t;
    t.init();
    t.startGrid();
    const int64_t beacon_t0 = t.nextBeaconT0Us(esp_timer_get_time());
    t.serviceBeacon();
    ASSERT_EQ(t.beaconsSent(), 1u);
    const int64_t fire = loratiming::fireInstantUs(beacon_t0, 0);
    proto_sim_timer_set_now_us(fire - LORATracker::kPrepareLeadUs);
    ASSERT_TRUE(t.serviceTxQueue(fire - LORATracker::kPrepareLeadUs));
    std::ostringstream s;
    s << "netkeyid=" << t.netKeyId() << "\n" << dumpRecorder();
    EXPECT_TRUE(golden::matches("tracker_grid_beacon", s.str()));
}

TEST(GoldenTrackerFrames, UnplacedBurstRestampsEveryCopy) {
    pin();
    LORATracker t;
    t.startGrid();
    proto_sim_timer_set_now_us(5'123'457);
    auto f = plaintextOperation();
    t.sendPacketBurst(f.data(), f.size(), /*copies=*/3, /*stride_ms=*/40);
    EXPECT_TRUE(golden::matches("tracker_unplaced_burst_restamp", dumpRecorder()));
}

TEST(GoldenTrackerFrames, PlacedSingleCopyOnMark) {
    pin();
    LORATracker t;
    t.startGrid();
    const int64_t mark = t.nextT0ForSlotUs(18, 2'000'000);
    proto_sim_timer_set_now_us(mark - 100'000);
    auto f = plaintextOperation();
    t.sendPacketBurst(f.data(), f.size(), /*copies=*/1, /*stride_ms=*/0,
                      /*not_before_us=*/loratiming::fireInstantUs(mark, 0), /*on_mark=*/true);
    EXPECT_TRUE(golden::matches("tracker_placed_single_on_mark", dumpRecorder()));
}

TEST(GoldenTrackerFrames, UngriddedFrameIsNotStamped) {
    pin();
    LORATracker t;
    auto f = plaintextOperation();
    t.sendPacketBurst(f.data(), f.size(), /*copies=*/2, /*stride_ms=*/0);
    EXPECT_TRUE(golden::matches("tracker_ungridded_burst", dumpRecorder()));
}
