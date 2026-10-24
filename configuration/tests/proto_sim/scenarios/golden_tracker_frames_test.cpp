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
#include "MacWire.h"
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

// S8: the same plaintext cover op as a v1 frame (PORT 2, AUTH = NONE); these
// scenarios then record into golden_v1/.
std::vector<uint8_t> plaintextOperation() {
    LoraCoverOperation op = LORA_COVER_OPERATION__INIT;
    op.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
    op.operation  = COV_OPERATION__CMD_OPEN;
    HubAppPdu pdu = HUB_APP_PDU__INIT;
    pdu.pdu_case  = HUB_APP_PDU__PDU_OPERATION;
    pdu.operation = &op;
    macwire::WireHdr h;
    h.flags = macwire::kFlagAckReq;
    h.dst = 18; h.src = 1; h.subnet = 2; h.fcnt = 42; h.port = macwire::kPortApp;
    const size_t hlen = macwire::headerLen(h);
    std::vector<uint8_t> buf(hlen + hub_app_pdu__get_packed_size(&pdu));
    macwire::encodeHeader(h, buf.data(), buf.size());
    hub_app_pdu__pack(&pdu, buf.data() + hlen);
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
