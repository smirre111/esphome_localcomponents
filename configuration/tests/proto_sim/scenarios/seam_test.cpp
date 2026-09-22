// ---------------------------------------------------------------------------
// THE SEAM — the real hub and the real node in one process.
//
// Nothing linked both before this file. real_lora_client_test drives the real
// LORAListener; real_cmd_dispatcher_test drives the real CmdDispatcher; the
// end-to-end scenarios drive hand-written mirrors (sim/hub_model.cpp,
// sim/node_model.cpp) that agree with each other by construction. So every
// question of the form "does the frame the hub actually sends arrive where the
// node is actually listening" had no home, and three separate defects lived
// there at once — a 3136 us placement error, a single copy aimed at a window
// that was never opened, and an anchor solved from a frame that did not occupy
// the slot it declared.
//
// Each of those was guarded by a passing test that restated ONE side's own
// arithmetic. `EXPECT_EQ(last_earliest_us, t0 + kRx1DelayUs)` is the expression
// send_into_rx1_() evaluates, so it agreed with the code while the frame landed
// 3136 us late at the node. A test can only catch that by asking the OTHER side
// where it is listening.
//
// The join between them is sim/air_channel.h, whose catch predicate is stated
// physically — a frame is caught if it starts after the window opens and
// completes detection before it closes — rather than as a restatement of the
// guard band. That is what makes the guard band checkable here instead of
// assumed.
// ---------------------------------------------------------------------------

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/homeassistant/time/homeassistant_time.h"

#include "CmdDispatcher.h"
#include "MotorCtrl.h"
#include "SystemCtrl.h"
#include "LoraInterface.h"

#include "blinds.pb-c.h"
#include "TimedGrid.h"
#include "LoraTiming.h"
#include "ClassAWindows.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/air_channel.h"

#include <psa/crypto.h>
#include <gtest/gtest.h>

using esphome::lora_tracker::LORATracker;
using esphome::lora_tracker::LORAClient;
using esphome::time::RealTimeClock;

// The node's esp_timer.h is included first (its shims come BEFORE on the
// include path) and does not declare the hub's two setters. Same symbols, same
// implementation — the seam target links the hub's esp_timer stub for both
// halves; see PROTO_SIM_EXTERNAL_ESP_TIMER in shims_node/esp_idf_stubs.c.
extern "C" void proto_sim_timer_set_now_us(int64_t us);

namespace {

constexpr uint8_t  kAddr   = 18;
constexpr uint8_t  kSubnet = 2;
constexpr uint64_t kMac    = 0x0011223344ULL;

// Both halves, one clock, one air.
struct Seam : public ::testing::Test {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;

    // Hub
    LORATracker   tracker;
    LORAClient    rol;
    RealTimeClock ha_time;

    // Node
    MotorCtrl     mot;
    SystemCtrl    sys;
    LoraInterface lif;
    portMUX_TYPE  motorMux{};
    portMUX_TYPE  buttonMux{};
    CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};

    void SetUp() override {
        ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        proto_sim_timer_reset();

        rol.set_name("rol");
        rol.set_short_address(kAddr);
        rol.set_subnet_address(kSubnet);
        rol.set_sleep_duration(21600);
        rol.set_address(kMac);
        ha_time.set_now(1787000000, /*valid=*/true);
        rol.set_time(&ha_time);
        tracker.register_client(&rol);
        rol.registered_ = true;

        // The node answers to the address the hub is addressing.
        sys.setAddress(kAddr, kSubnet);
    }

    void TearDown() override {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }

    // The most recent hub->node frame the tracker emitted.
    std::vector<uint8_t> lastDownlink() const {
        const auto &f = radio.hub_to_node_frames();
        return f.empty() ? std::vector<uint8_t>{} : f.back().bytes;
    }

    // Deliver a frame to the node as the radio would: the node is handed
    // RxDone, and recovers T0 itself. Handing it a T0 directly would skip the
    // one conversion most likely to be wrong.
    void deliverAtT0(const std::vector<uint8_t> &bytes, int64_t t0_us) {
        const int64_t rxdone =
            t0_us + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) bytes.size());
        disp.onReceiveNew(const_cast<uint8_t *>(bytes.data()),
                          (int) bytes.size(), rxdone);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
TEST_F(Seam, AGridAlignedDownlinkArrivesInsideTheNodesWindow) {
    // The end-to-end statement B1a/B3 are actually for, and the one no test
    // could make: the hub places a frame on the grid, the node arms a window
    // from the grid it was given, and the frame is CAUGHT.
    //
    // Both halves are production arithmetic. The hub's mark comes from
    // nextClearT0ForSlotUs and its fire instant from fireInstantUs; the node's
    // window comes from nextArmInstantUs off the anchor it solved from a real
    // GridSync frame. Nothing here recomputes either side's expression.
    proto_sim_timer_set_now_us(1'000'000);
    tracker.startGrid();
    rol.enable_timed_mode(true);
    ASSERT_TRUE(tracker.gridStarted());

    // The GridSync the hub really packed, delivered at the mark it really
    // declares. send_grid_sync says txround = 0, txslot = grid_slot_, so this
    // is the frame occupying the position it claims — see the next test for
    // what happens when it does not.
    const auto gridsync = lastDownlink();
    ASSERT_FALSE(gridsync.empty()) << "enable_timed_mode must publish a GridSync";
    const int64_t declared_mark =
        tracker.nextT0ForSlotUs(rol.grid_slot(), tracker.gridAnchorUs());
    deliverAtT0(gridsync, declared_mark);
    // Adoption, not promotion. timedRxActive() additionally requires a
    // trustworthy phase built from a long baseline of addressed frames — that
    // is the policy question of whether the node should USE the grid. What is
    // under test here is the geometry: given a node that HAS the grid, does the
    // hub's frame land where the node's arm arithmetic points?
    ASSERT_TRUE(disp.gridState().active)
        << "the node must have adopted the grid from the hub's own frame";

    // Now a real addressed downlink, placed by the hub's real path. A cover
    // operation rather than raw bytes: the node commits a phase sample only for
    // a frame that parses and is addressed to it, and that sample is the
    // measuring instrument below.
    proto_sim_timer_set_now_us(declared_mark + 1000);
    const size_t before = radio.hub_to_node_frames().size();
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_OPEN, 0.0f);
    ASSERT_GT(radio.hub_to_node_frames().size(), before)
        << "the hub must have emitted the command";

    const int64_t fire = tracker.last_earliest_us;
    ASSERT_GT(fire, 0) << "the downlink must have been placed, not sent bare";

    // What the node will see. earliest_us is the instant lora_tx() is called —
    // the first chirp — so the reference T0 the node recovers is
    // kPreambleToT0Us later. This is the one conversion the hub-side tests
    // could not check, because they asserted the hub's own expression.
    const auto     frame = lastDownlink();
    const int64_t  seen_t0 = fire + (int64_t) loratiming::kPreambleToT0Us;

    // (1) The geometric statement: is it caught at all?
    const proto_sim::Transmission tx{
        seen_t0, (uint32_t) frame.size(), /*tx_id=*/1, /*seq=*/0};
    const int64_t arm = disp.nextArmInstantUs(seen_t0 - 1'000'000);
    ASSERT_GT(arm, 0) << "a node on a grid must have a window to arm";
    const proto_sim::RxWindow win{arm, timedgrid::kWindowUs};
    EXPECT_TRUE(proto_sim::caught(win, tx, timedgrid::kDetectUs))
        << "hub fired at " << fire << " (T0 " << seen_t0 << "), node window ["
        << win.open_us << ", " << win.close_us() << ")";

    // (2) The tight statement, and the one that matters. "Caught" has 14 ms of
    // slack on each side, so a systematic error of a few milliseconds passes it
    // while quietly eating the guard band — a 3136 us placement bug did exactly
    // that and survived review. The node's OWN phase machinery is the right
    // instrument: deliver the frame and read back what the node measured
    // against the mark it was expecting. This is phaseErrUs, the number the
    // field gate gives as +/-2 ms.
    const uint32_t n_before = disp.phaseStats().n;
    deliverAtT0(frame, seen_t0);
    ASSERT_GT(disp.phaseStats().n, n_before)
        << "the node must have committed a phase sample for this frame";

    EXPECT_EQ(disp.phaseStats().last_us, 0)
        << "the hub's placed frame must land ON the node's mark. A non-zero "
           "value here is a SYSTEMATIC offset between the two ends, spent out "
           "of the guard band before the link has done anything";
    EXPECT_EQ(disp.phaseStats().outside_guard, 0u);
}

// ---------------------------------------------------------------------------
TEST_F(Seam, AnUnplacedGridSyncDisplacesEveryMarkTheNodeWillEverArm) {
    // The mechanism behind the largest open defect in the plan (§11b, "found by
    // review and NOT fixed").
    //
    // send_grid_sync() declares `txround = 0, txslot = grid_slot_` and the node
    // solves its anchor from that declaration plus the T0 it MEASURED — so the
    // declaration is a promise about where the frame will be. Its own comment
    // says as much: "it must describe where the frame will ACTUALLY be
    // transmitted ... Declaring a position the frame does not occupy would
    // anchor every node wrong, so the grid is published only from an aligned
    // client."
    //
    // Nothing enforces that. send_grid_sync calls parent_->send() directly, not
    // send_aligned_, so the frame leaves whenever sendTask reaches it and as a
    // 17-copy burst whose copies are 88000 us apart — a stride that is not a
    // multiple of the 46875 us slot pitch, so the copies walk across slot
    // boundaries and the node adopts from whichever it decodes first.
    //
    // This test does not assert that the bug exists; it states the mechanism,
    // which stays true either way: the node's anchor follows ARRIVAL, so the
    // displacement of the GridSync becomes the standing error on every mark.
    // When send_grid_sync is placed, the displacement is zero and so is the
    // error — the assertions below are written in terms of `skew` for exactly
    // that reason.
    constexpr int64_t skew = 40'000;   // 40 ms of queue delay: well under one
                                       // round, and three times the guard band

    proto_sim_timer_set_now_us(1'000'000);
    tracker.startGrid();
    rol.enable_timed_mode(true);
    ASSERT_TRUE(tracker.gridStarted());

    const auto gridsync = lastDownlink();
    ASSERT_FALSE(gridsync.empty());
    const int64_t declared_mark =
        tracker.nextT0ForSlotUs(rol.grid_slot(), tracker.gridAnchorUs());

    // The frame arrives `skew` after the mark it claims to occupy.
    deliverAtT0(gridsync, declared_mark + skew);
    ASSERT_TRUE(disp.gridState().active);

    // A downlink the hub places on ITS grid, correctly.
    proto_sim_timer_set_now_us(declared_mark + skew + 1000);
    const size_t before = radio.hub_to_node_frames().size();
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_OPEN, 0.0f);
    ASSERT_GT(radio.hub_to_node_frames().size(), before);

    const int64_t fire    = tracker.last_earliest_us;
    const int64_t seen_t0 = fire + (int64_t) loratiming::kPreambleToT0Us;
    const auto    frame   = lastDownlink();

    // The node measures the hub's correctly-placed frame against its own
    // displaced marks, and reports the displacement.
    const uint32_t n_before = disp.phaseStats().n;
    deliverAtT0(frame, seen_t0);
    ASSERT_GT(disp.phaseStats().n, n_before);
    EXPECT_EQ(disp.phaseStats().last_us, -(int32_t) skew)
        << "the node's anchor follows where the GridSync ARRIVED, so publishing "
           "it from an unplaced frame moves every future mark by that much";

    // And the consequence, which is the point: past the guard band the frame is
    // not merely late, it is missed. The node arms a window the hub never
    // transmits into, on every round, while both ends believe they agree.
    ASSERT_GT(skew, (int64_t) timedgrid::kGuardUs);
    const proto_sim::Transmission tx{
        seen_t0, (uint32_t) frame.size(), /*tx_id=*/1, /*seq=*/0};
    const int64_t arm = disp.nextArmInstantUs(seen_t0 - 1'000'000);
    const proto_sim::RxWindow win{arm, timedgrid::kWindowUs};
    EXPECT_FALSE(proto_sim::caught(win, tx, timedgrid::kDetectUs))
        << "a displacement larger than the guard band must miss — if this "
           "starts passing, the guard band and the catch predicate disagree";
    EXPECT_EQ(disp.phaseStats().outside_guard, 1u)
        << "and the node's own promotion criterion must see it";
}
