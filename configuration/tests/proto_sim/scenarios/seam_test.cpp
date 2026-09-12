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
#include "GridState.h"
#include "PendingData.h"

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

    // The GridSync the hub really packed, delivered where the hub really put
    // it. Nothing here chooses the instant: send_grid_sync declares
    // txround = 0, txslot = grid_slot_, and the frame must OCCUPY that slot for
    // the declaration to be true.
    const auto gridsync = lastDownlink();
    ASSERT_FALSE(gridsync.empty()) << "enable_timed_mode must publish a GridSync";

    const int64_t gs_fire = tracker.last_earliest_us;
    ASSERT_GT(gs_fire, 0)
        << "the GridSync must be PLACED. Sent bare, it leaves whenever the "
           "queue drains and declares a slot it does not occupy";
    const int64_t gs_t0 = gs_fire + (int64_t) loratiming::kPreambleToT0Us;
    EXPECT_EQ(tracker.nextT0ForSlotUs(rol.grid_slot(), gs_t0 - 1), gs_t0)
        << "copy 0 of the GridSync must land ON this node's mark, since that is "
           "what the frame claims and what the node will anchor to";
    EXPECT_NE(tracker.last_copies, 1)
        << "and it must still be a burst: a node being told about the grid is "
           "by definition not yet on it, so it is sweeping a free-running "
           "window and would miss a single copy most of the time";

    deliverAtT0(gridsync, gs_t0);
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
    proto_sim_timer_set_now_us(gs_t0 + 1000);
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

// ---------------------------------------------------------------------------
TEST_F(Seam, TheHubsRealBeaconVerifiesOnTheRealNode) {
    // Section 4.4's authenticator, across the seam. Both the hub-side and the
    // node-side tests call framecrypto::buildBeaconMacInput, so neither can
    // catch the two ends disagreeing about the LAYOUT — they agree by
    // construction, which is the same shape of blind spot the placement tests
    // had. What only this test can say is that the tag the hub really put on
    // the air verifies against the key the node really adopted.
    //
    // Three separate things have to line up for that, and each has failed in
    // some form in this system already: the key has to reach the node at all
    // (it rides an ENCRYPTED GridSync, so a plaintext publish carries none),
    // the truncation convention has to match (the hub computes a full CMAC and
    // takes eight bytes; the node verifies with PSA_ALG_TRUNCATED_MAC), and the
    // beacon has to be delivered on a mark the node predicts.
    proto_sim_timer_set_now_us(1'000'000);
    tracker.startGrid();
    ASSERT_NE(tracker.netKeyId(), 0u) << "startGrid must mint a fleet key";

    // A real session, because the key is only carried on an encrypted GridSync
    // and only adopted from an authenticated one. send_login() mints the base
    // nonce and packs the real LoginMsg; the node adopts it from that frame.
    // config_synced_ suppresses the request_register flag. Without it the
    // node answers the challenge with a REGISTER instead of adopting the nonce,
    // which is correct behaviour for an unprovisioned node and not the path
    // under test here.
    rol.config_synced_ = true;
    rol.send_login();
    const auto login = lastDownlink();
    ASSERT_FALSE(login.empty()) << "send_login must have emitted a LoginMsg";
    disp.onReceiveNew(const_cast<uint8_t *>(login.data()), (int) login.size(),
                      esp_timer_get_time());
    uint32_t nonce = 0;
    ASSERT_TRUE(disp.getBaseNonceForTest(1, nonce))
        << "the node must hold the hub's base nonce before anything is encrypted";
    // What the hub's own ack path would set once the node answers the challenge.
    rol.session_confirmed_ = true;

    // Now the grid. Encrypted, so it carries the key.
    rol.enable_timed_mode(true);
    const auto gridsync = lastDownlink();
    ASSERT_FALSE(gridsync.empty());
    const int64_t gs_t0 =
        tracker.last_earliest_us + (int64_t) loratiming::kPreambleToT0Us;
    deliverAtT0(gridsync, gs_t0);

    ASSERT_TRUE(disp.gridState().active);
    ASSERT_TRUE(disp.hasNetKey())
        << "an ENCRYPTED GridSync is what carries the fleet key; a plaintext "
           "one carries none, and the beacon's MAC would then be decorative";
    EXPECT_EQ(disp.netKeyId(), tracker.netKeyId())
        << "and it must be the key THIS hub minted, not merely some key";

    // The beacon. Built here rather than pulled off the air, because the SHIM
    // tracker has no serviceBeacon — that path is covered against the real
    // tracker in real_lora_tracker_test, which unpacks the frame it queues and
    // re-verifies its tag. What only THIS test can add is the other half: the
    // tag is produced by the hub-side convention (full CMAC, first eight bytes)
    // and checked by the node's real verify, which asks PSA for a TRUNCATED
    // MAC. Those are two different algorithm identifiers over the same key, and
    // nothing else in the suite puts them on opposite sides of one assertion.
    const uint32_t round = 0;
    const uint32_t slot  = disp.gridState().params.beacon_slot;
    const uint32_t mask  = pending::allListening();

    uint8_t mac[framecrypto::kBeaconMacBytes];
    ASSERT_TRUE(tracker.beaconMac(round, slot, mask, true, mac, sizeof(mac)))
        << "a hub that cannot sign sends no beacon at all";

    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = LORATracker::broadcastAddressing;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = 0;      // a broadcast belongs to no per-node sequence
    hdr.burstcount    = 1;

    GridBeacon gb = GRID_BEACON__INIT;
    gb.txround          = round;
    gb.txslot           = slot;
    gb.pendingmask      = mask;
    gb.pendingmaskvalid = true;
    gb.netkeyid         = tracker.netKeyId();
    gb.mac.data         = mac;
    gb.mac.len          = sizeof(mac);

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header     = &hdr;
    op.cmd_case   = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON;
    op.gridbeacon = &gb;
    std::vector<uint8_t> beacon(
        lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, beacon.data());

    // Delivered on the node's OWN predicted beacon mark, recovered from the
    // anchor the node solved rather than from the hub's — which is the whole
    // reason the anchor is solved locally instead of transferred.
    deliverAtT0(beacon, gridstate::beaconT0ForRound(disp.gridState(), round));

    EXPECT_TRUE(disp.pendingStateForTest().valid)
        << "the node adopts a bitmap ONLY from a beacon whose MAC verified, so "
           "this is the end-to-end statement: the hub's tag checked out against "
           "the key the node got over the encrypted channel";
    EXPECT_EQ(disp.pendingStateForTest().bits, mask);
}

// ---------------------------------------------------------------------------
TEST_F(Seam, AProvisionedNodeAndAFreshlyBootedHubMustNotLoopRegisterAgainstLogin) {
    // THE DEADLOCK THAT COST THE FIRST BENCH SESSION ITS FIRST HOUR.
    //
    // Observed live: node 2 provisioned, hub freshly flashed, radio healthy at
    // RSSI -36 / SNR 5.75 — and the two ends traded REGISTER against LoginMsg
    // for eight minutes without ever establishing a session. Every measurement
    // in bench-runbook.md needs a session, so nothing could run at all.
    //
    // The cycle, with each step being correct in isolation:
    //
    //   handle_register_  the node reports itself PROVISIONED, so the config
    //                     push is DEFERRED to confirm_session_() — a provisioned
    //                     node refuses plaintext config, so this is right — and
    //                     config_synced_ stays false
    //   send_login        request_register = !config_synced_ = true
    //   the node          answers request_register with a plaintext REGISTER and,
    //                     BY DESIGN, does not store the nonce or ack the login:
    //                     it expects the next login to carry the flag clear
    //   handle_register_  provisioned again -> defer again -> still unsynced
    //
    // confirm_session_() is the only place that clears config_push_pending_ and
    // sets config_synced_, and it runs only on an encrypted uplink — which
    // request_register just told the node not to send. Neither end is wrong
    // about its own job; the hub is asking a provisioned node to re-register
    // while waiting for the session that re-registering tears down.
    //
    // This is a seam test because neither half can see it. The hub's own tests
    // assert that request_register follows config_synced_ (it does). The node's
    // own tests assert that request_register produces a REGISTER (it does).
    // Only the two together loop.
    proto_sim_timer_set_now_us(1'000'000);

    ASSERT_TRUE(disp.isProvisioned())
        << "the bench node holds an address, which is what makes the hub defer "
           "rather than push in the clear";
    ASSERT_FALSE(rol.config_synced_)
        << "a freshly booted or flashed hub has pushed nothing this boot — "
           "this is the state the deadlock needs, and a hub reflash creates it";

    // The node's real REGISTER, into the real hub.
    disp.sendRegister();
    // sendRegister() only sets the pending status; runOneTxCommand() is the
    // node's real transmit body, which is what actually builds the frame.
    while (disp.runOneTxCommand()) {}
    auto up = lif.drain_tx_queue();
    ASSERT_FALSE(up.empty()) << "the node must have transmitted a REGISTER";

    // The hub only accepts a REGISTER whose MAC matches the one it is
    // configured for, so take the MAC from the node's own frame rather than
    // assuming the fixture's. Getting this wrong makes the hub IGNORE the
    // register and the test then fails for a setup reason that looks like the
    // defect.
    {
        auto *r = lora_client_response_message__unpack(NULL, up.back().size(),
                                                       up.back().data());
        ASSERT_NE(r, nullptr);
        ASSERT_EQ(r->proto_case, LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER);
        rol.set_address(r->register_->mac_addr);
        lora_client_response_message__free_unpacked(r, NULL);
    }

    rol.set_response(up.back().data(), up.back().size());

    // The hub defers, as it should for a provisioned node.
    EXPECT_TRUE(rol.config_push_pending_)
        << "a provisioned node's config push must wait for encryption";
    EXPECT_FALSE(rol.config_synced_)
        << "and a push that has not happened must not be recorded as one that "
           "did — config_synced_ is set in confirm_session_(), not here";

    // The login that follows. THE FLAG IS THE WHOLE TEST.
    rol.send_login();
    const auto login = lastDownlink();
    ASSERT_FALSE(login.empty()) << "send_login must have emitted a LoginMsg";

    auto *msg = lora_client_operation_message__unpack(NULL, login.size(), login.data());
    ASSERT_NE(msg, nullptr) << "the LoginMsg goes out in the clear and must unpack";
    ASSERT_EQ(msg->cmd_case, LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN);
    const bool asked_to_reregister = msg->login->request_register;
    lora_client_operation_message__free_unpacked(msg, NULL);

    EXPECT_FALSE(asked_to_reregister)
        << "while a config push is pending, the hub must ask for a LOGIN and "
           "nothing else. request_register here is the deadlock: the node "
           "answers it with a REGISTER instead of the encrypted ack that "
           "confirm_session_() needs to send the pending push";

    // And the node's actual reaction, which is the half the hub cannot assert.
    lif.drain_tx_queue();   // ignore anything queued before this point
    deliverAtT0(login, esp_timer_get_time() + 100'000);
    while (disp.runOneTxCommand()) {}

    uint32_t nonce = 0;
    EXPECT_TRUE(disp.getBaseNonceForTest(1, nonce))
        << "the node must ADOPT the hub's base nonce, which is what lets it "
           "encrypt the uplink that confirms the session. Answering with a "
           "REGISTER instead leaves it with no nonce and the link wedged";

    const auto after = lif.drain_tx_queue();
    bool sent_register = false;
    for (const auto &f : after) {
        // The node's REGISTER is a RESPONSE message, not an operation: it is
        // what set_response() on the hub consumes.
        auto *m = lora_client_response_message__unpack(NULL, f.size(), f.data());
        if (m == nullptr) continue;
        if (m->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
            sent_register = true;
        lora_client_response_message__free_unpacked(m, NULL);
    }
    EXPECT_FALSE(sent_register)
        << "a second REGISTER here IS the loop: it returns the hub to "
           "handle_register_, which defers again, and the two ends never "
           "converge";
}
