// ---------------------------------------------------------------------------
// END TO END — the real hub and the real node holding a conversation.
//
// seam_test.cpp asserts GEOMETRY: does the frame the hub places land inside the
// window the node armed. This file asserts the CONVERSATIONS that geometry
// exists to carry — login to a proven session, a command to its ack, a lost ack
// recovered, a superseded command, a schedule push confirmed.
//
// WHY IT DID NOT EXIST. Every end-to-end scenario before this drove hand-written
// mirrors (sim/hub_model.cpp, sim/node_model.cpp) which agree with each other by
// construction, so a defect present in both was invisible to all of them. And
// the node's uplink path could not be reached at all: processTxCommand — where
// every frame the node sends is built, stamped and packed — is a `for(;;)`
// blocking on portMAX_DELAY. Its body is now a function (serviceTxCommand), so
// the loop below drives the real one.
//
// NO HOME ASSISTANT, and no ESPHome entity. The hub is driven through exactly
// the API its automations call — send_cover_operation, send_schedule_config,
// send_login — and everything asserted is protocol state on one side or the
// other. Nothing here reads a sensor or publishes one.
//
// WHAT THIS HARNESS CANNOT REACH, stated so its green result is not read as
// more than it is. The hub side is the SHIM tracker, which emits from send()
// with no buffer pool and no transmit queue — so nothing here exercises frame
// PLACEMENT, the prepare/fire split, or the supersession that drops a frame at
// the front of the queue. Those need the real LORATracker and are tested
// against it in real_lora_tracker_test; what this file adds is the half that
// target cannot have, a real node at the other end.
//
// THE ONE RULE these tests are written to. Never assert one side's arithmetic
// against an expression copied from that same side. Ask the OTHER end what it
// saw. That is the mistake §11b's seam entry exists to record, and it is why
// `EXPECT_EQ(last_earliest_us, <the expression send_into_rx1_ evaluates>)`
// passed for months while the frame landed 3136 µs late.
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
#include "GridState.h"
#include "PendingData.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"

#include <psa/crypto.h>
#include <gtest/gtest.h>

#include <vector>

using esphome::lora_tracker::LORATracker;
using esphome::lora_tracker::LORAClient;
using esphome::time::RealTimeClock;

extern "C" void proto_sim_timer_set_now_us(int64_t us);

namespace {

constexpr uint8_t  kAddr   = 18;
constexpr uint8_t  kSubnet = 2;
constexpr uint64_t kMac    = 0x0011223344ULL;
constexpr uint8_t  kHub    = 1;

// Both ends, one clock, one air — plus a message loop between them.
struct E2E : public ::testing::Test {
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

    // How many hub->node frames the loop has already handed over, so each pump
    // delivers only what is new.
    size_t delivered_{0};
    // Frames the loop was told to lose, by index in the radio transcript.
    std::vector<size_t> drop_downlink_;
    // Uplinks to swallow on their way to the hub, as a countdown.
    int drop_uplinks_{0};

    void SetUp() override {
        ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        proto_sim_timer_reset();
        proto_sim_timer_set_now_us(1'000'000);

        rol.set_name("rol");
        rol.set_short_address(kAddr);
        rol.set_subnet_address(kSubnet);
        rol.set_sleep_duration(21600);
        rol.set_address(kMac);
        ha_time.set_now(1787000000, /*valid=*/true);
        rol.set_time(&ha_time);
        tracker.register_client(&rol);

        // A provisioned, registered node: the state a fleet is in for all but
        // the first minute of its life. Bootstrap has its own tests.
        rol.registered_    = true;
        rol.config_synced_ = true;
        sys.setAddress(kAddr, kSubnet);
    }

    void TearDown() override {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }

    // --- the loop ---------------------------------------------------------

    // Hand every hub frame not yet delivered to the node, at the T0 the hub
    // actually aimed for. The node recovers T0 itself from RxDone, which is the
    // one conversion most likely to be wrong, so it is never skipped.
    int pumpDown() {
        const auto &f = radio.hub_to_node_frames();
        int n = 0;
        for (; delivered_ < f.size(); ++delivered_) {
            bool lost = false;
            for (size_t idx : drop_downlink_)
                if (idx == delivered_) lost = true;
            if (lost) continue;
            const auto &bytes = f[delivered_].bytes;
            const int64_t t0 = (tracker.last_earliest_us > 0)
                ? tracker.last_earliest_us + (int64_t) loratiming::kPreambleToT0Us
                : esp_timer_get_time();
            const int64_t rxdone =
                t0 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) bytes.size());
            disp.noteDriftSample(rxdone);
            disp.onReceiveNew(const_cast<uint8_t *>(bytes.data()),
                              (int) bytes.size(), rxdone);
            ++n;
        }
        return n;
    }

    // Run the node's real transmit body until its queue is empty, then hand
    // whatever it produced to the hub.
    int pumpUp() {
        int built = 0;
        while (disp.runOneTxCommand()) ++built;
        int sent = 0;
        for (auto &frame : lif.drain_tx_queue()) {
            if (drop_uplinks_ > 0) { --drop_uplinks_; continue; }
            // The hub stamps where this node's uplink landed; a test that cares
            // about in-slot placement sets it itself before pumping.
            rol.set_response(frame.data(), frame.size());
            ++sent;
        }
        (void) built;
        return sent;
    }

    // Both directions until nothing moves. Bounded: a loop that will not settle
    // is a finding, not something to spin on.
    void settle(int max_rounds = 8) {
        for (int i = 0; i < max_rounds; ++i) {
            const int down = pumpDown();
            const int up   = pumpUp();
            if (down == 0 && up == 0) return;
        }
    }

    // A full session, the way it really happens: the hub challenges, the node
    // adopts the nonce and answers, the hub confirms.
    void bringUpSession() {
        rol.send_login();
        settle();
    }

    // What the node queued for its motor task — "did it actually act on the
    // command", asked of the node rather than inferred from the ack.
    std::vector<int> drainNodeOps() {
        std::vector<int> out;
        blinds_syscmd_base_t c;
        while (xQueueReceive(disp.rxCmdQueueNew, &c, 0) == pdTRUE)
            out.push_back((int) c);
        return out;
    }

    // BOTH clocks, and that is not a convenience.
    //
    // clock.tick() drives the ESPHome scheduler, which is what fires the hub's
    // retry timeout. esp_timer_get_time() is a separate settable clock, and it
    // is what the NODE reads — its AckCache discriminates a re-ack from a
    // burst duplicate purely on elapsed time. Advancing only the first makes
    // the hub retry while the node still believes no time has passed, so it
    // answers a legitimate retry as a burst duplicate and stays silent. The
    // first draft of this fixture did exactly that and two tests failed in a
    // way that looked like a product defect.
    void tick(uint32_t ms) {
        proto_sim_timer_set_now_us(esp_timer_get_time() + (int64_t) ms * 1000);
        clock.tick(ms);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. The session
// ---------------------------------------------------------------------------

TEST_F(E2E, ALoginBecomesAProvenSessionOnBothSides) {
    // The handshake that everything else rests on, and the first time it has
    // run across real code in both directions. The hub's own tests stop at
    // "a LoginMsg was emitted"; the node's stop at "a nonce was stored".
    EXPECT_FALSE(disp.isSessionProven());

    bringUpSession();

    uint32_t nonce = 0;
    ASSERT_TRUE(disp.getBaseNonceForTest(kHub, nonce))
        << "the node must hold the hub's base nonce";
    EXPECT_NE(nonce, 0u);

    EXPECT_TRUE(rol.login_acked_)
        << "and the hub must have seen the node answer its challenge";
}

TEST_F(E2E, TheNodeAnswersTheChallengeWithARealUplink) {
    // Not "the queue has an entry" — an actual frame, built by the real
    // transmit body, parsed here as the hub would parse it.
    rol.send_login();
    ASSERT_GT(pumpDown(), 0) << "the challenge must reach the node";

    while (disp.runOneTxCommand()) { }
    auto frames = lif.drain_tx_queue();
    ASSERT_FALSE(frames.empty()) << "the node must transmit a reply";

    LoraClientResponseMessage *msg = lora_client_response_message__unpack(
        nullptr, frames[0].size(), frames[0].data());
    ASSERT_NE(msg, nullptr) << "and it must be a parseable response message";
    ASSERT_NE(msg->header, nullptr);
    EXPECT_EQ(msg->header->senderaddress, (uint32_t) kAddr);
    EXPECT_EQ(msg->header->destaddress, (uint32_t) kHub);
    lora_client_response_message__free_unpacked(msg, nullptr);
}

// ---------------------------------------------------------------------------
// 2. A command, and its ack
// ---------------------------------------------------------------------------

TEST_F(E2E, ACoverCommandIsExecutedAndAcknowledged) {
    // The round trip the product is. The hub sends OPEN, the node acts on it,
    // the node acks, and the hub stops waiting. Every link in that chain was
    // covered on one side only.
    bringUpSession();
    drainNodeOps();   // the login path queues its own status work

    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_OPEN, 0.0f);
    ASSERT_TRUE(rol.awaitingAck()) << "precondition: the hub is tracking it";

    settle();

    const auto ops = drainNodeOps();
    ASSERT_FALSE(ops.empty())
        << "the node must have queued the operation for its motor";
    EXPECT_EQ(ops.front(), (int) BlindsOpCmd::SYSCMD_OPEN)
        << "and it must be the operation the hub actually sent";

    EXPECT_FALSE(rol.awaitingAck())
        << "the node's ack must have reached the hub and closed the command";
    EXPECT_FALSE(rol.commandFailed());
}

TEST_F(E2E, ACloseIsNotAnOpen) {
    // The cheapest possible way for the two ends to disagree, and one nothing
    // checked: an enum that survives the wire in the wrong direction would look
    // like a working link in every other test here.
    bringUpSession();
    drainNodeOps();

    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_CLOSE, 0.0f);
    settle();

    const auto ops = drainNodeOps();
    ASSERT_FALSE(ops.empty());
    EXPECT_EQ(ops.front(), (int) BlindsOpCmd::SYSCMD_CLOSE);
}

TEST_F(E2E, APositionCommandCrossesTheWireAsThePositionAsked) {
    bringUpSession();
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.42f);
    settle();

    EXPECT_NEAR(mot.target_position(), 0.42f, 0.01f)
        << "the node's motor target is the only honest witness that a float "
           "survived the protobuf round trip";
    EXPECT_FALSE(rol.awaitingAck());
}

// ---------------------------------------------------------------------------
// 3. Recovery
// ---------------------------------------------------------------------------

TEST_F(E2E, ALostAckIsRecoveredByTheRetryAndTheNodeDoesNotMoveTwice) {
    // B4's whole reason for existing, end to end. The hub retransmits the
    // BYTE-IDENTICAL frame (pack-once), so the node sees a duplicate msgid —
    // and must answer it again rather than drop it as a replay, WITHOUT
    // executing the command a second time. A blind that moves twice is the
    // failure this pair of mechanisms was built to prevent.
    bringUpSession();
    drainNodeOps();

    drop_uplinks_ = 1;   // the node's first ack never arrives
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_OPEN, 0.0f);
    settle();

    ASSERT_EQ(drainNodeOps().size(), (size_t) 1) << "executed once so far";
    EXPECT_TRUE(rol.awaitingAck()) << "the hub is still waiting, correctly";

    // The retry fires.
    tick(3000 + 50);   // kOpRetryIntervalMs
    settle();

    EXPECT_TRUE(drainNodeOps().empty())
        << "the duplicate must NOT be executed again — that is a blind that "
           "moves twice from one command";
    EXPECT_FALSE(rol.awaitingAck())
        << "but it must be re-acked, or the lost ack is unrecoverable and the "
           "hub tears down a session that was working";
    EXPECT_FALSE(rol.commandFailed());
}

TEST_F(E2E, ACommandLostOnAirIsRetriedUntilItLands) {
    // The other direction of loss. The downlink itself never arrives, so the
    // node has nothing to ack and the hub's retry ladder is what saves the
    // command.
    bringUpSession();
    drainNodeOps();

    drop_downlink_.push_back(radio.hub_to_node_frames().size());  // the next one
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_OPEN, 0.0f);
    settle();
    ASSERT_TRUE(drainNodeOps().empty()) << "precondition: it was lost";
    ASSERT_TRUE(rol.awaitingAck());

    tick(3000 + 50);
    settle();

    const auto ops = drainNodeOps();
    ASSERT_FALSE(ops.empty()) << "the retry must carry the command through";
    EXPECT_EQ(ops.front(), (int) BlindsOpCmd::SYSCMD_OPEN);
    EXPECT_FALSE(rol.awaitingAck());
}

// ---------------------------------------------------------------------------
// 4. The grid
// ---------------------------------------------------------------------------

TEST_F(E2E, ANodeOnTheGridStillAnswersCommands) {
    // Mode B end to end: the hub publishes a grid, the node adopts it and arms
    // one window per round, and an ordinary command still completes. The
    // geometry is seam_test's; what this adds is that the CONVERSATION survives
    // the mode change.
    bringUpSession();
    drainNodeOps();

    rol.enable_timed_mode(true);
    settle();
    ASSERT_TRUE(disp.gridState().active)
        << "the node must have adopted the grid from the hub's own frame";
    ASSERT_TRUE(disp.timedRxEnabledForTest());

    drainNodeOps();
    rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                             COV_OPERATION__CMD_CLOSE, 0.0f);
    settle();

    const auto ops = drainNodeOps();
    ASSERT_FALSE(ops.empty()) << "a command must still arrive in Mode B";
    EXPECT_EQ(ops.front(), (int) BlindsOpCmd::SYSCMD_CLOSE);
    EXPECT_FALSE(rol.awaitingAck());
}

TEST_F(E2E, WithdrawingTheGridPutsTheNodeBackInModeA) {
    // The safe direction, and it has to work from the hub's own withdrawal
    // frame rather than from a test hook: a node left on a dead anchor arms
    // windows at marks that no longer exist.
    bringUpSession();
    rol.enable_timed_mode(true);
    settle();
    ASSERT_TRUE(disp.gridState().active);

    rol.enable_timed_mode(false);
    settle();

    EXPECT_FALSE(disp.gridState().active)
        << "the withdrawal must reach the node and clear its anchor";
    EXPECT_TRUE(disp.hasNetKey())
        << "but NOT the fleet key: a node with no key believes unsigned beacons "
           "again, and the guard bounds one anchor nudge rather than a sequence";
}

TEST_F(E2E, AHubRestartsAndItsBroadcastDemoteReachesAKeyedNode) {
    // D-1, end to end. The hub sends this frame on a FRESH BOOT, holding no
    // session, so it goes out in the clear — and a node holding a session
    // resumed from NVS refuses plaintext everything else. As
    // GridSync{enable=false} it was dropped by exactly the nodes it was aimed
    // at; as its own fieldless type it is exempt from that gate.
    bringUpSession();
    rol.enable_timed_mode(true);
    settle();
    ASSERT_TRUE(disp.gridState().active);
    ASSERT_TRUE(disp.hasNetKey());
    ASSERT_TRUE(disp.isSessionProven()) << "precondition: the gate is armed";

    // What the hub really emits on startup, through its own call.
    rol.broadcast_grid_demote();
    settle();

    EXPECT_FALSE(disp.gridState().active)
        << "the node must be back in Mode A — three windows per round, which is "
           "the only direction an unauthenticated frame may move it";
    EXPECT_TRUE(disp.hasNetKey())
        << "and its key must survive, or the demote is an escalation rather "
           "than a fallback";
}

// ---------------------------------------------------------------------------
// 5. The schedule
// ---------------------------------------------------------------------------

TEST_F(E2E, ASchedulePushIsAcknowledgedAndTheVersionAgrees) {
    // The largest frame on the link, and the only routine downlink the node
    // acks. The hub clears its retry on that ack and adopts the node's version
    // without waiting for a beacon, which is what makes "Schedule Pending"
    // mean anything.
    bringUpSession();

    rol.send_schedule_config();
    ASSERT_NE(rol.schedulePushMsgid(), 0u)
        << "precondition: the hub is tracking the push";

    settle();

    EXPECT_EQ(rol.schedulePushMsgid(), 0u)
        << "the node's ack must have cleared the outstanding push";
}

// ---------------------------------------------------------------------------
// 6. D-2: a lost reply into RX1, and why the hub does not retry into RX2
//
// The frame the hub aims at a Class A RX1 window is a TimeSync, and it does two
// jobs: it refreshes the node's clock, and — because it is encrypted — it is the
// node's proof that a resumed session actually works. If it is lost, the hub
// has no trigger to try RX2 with, because a TimeSync carries no ack.
//
// It does not need one. THE RECOVERY IS NODE-DRIVEN: armResumeFallback() starts
// a ladder of two re-beacons at ~4 s before escalating to a REGISTER at 12 s,
// and the hub answers EVERY beacon with a TimeSync. So the node re-ASKS rather
// than the hub guessing which window to re-aim at — strictly better
// information, and it is why a hub-side RX2 retry would be redundant.
//
// What makes that structural rather than lucky: sleepOk rides the TimeSync
// itself. A node that loses the reply never receives permission to sleep early,
// so it stays awake for the full quiet window — which is exactly the window the
// ladder needs. The failure keeps its own recovery alive.
// ---------------------------------------------------------------------------

TEST_F(E2E, ALostReplyLeavesTheSessionUnprovenSoTheLadderRuns) {
    // The precondition the whole argument rests on. A decrypted downlink is
    // what cancels the resume fallback (noteSessionProven); if the reply is
    // lost, the node must NOT believe its session is proven, because that
    // belief is what would stop it re-asking.
    bringUpSession();
    // NOT proven yet, and that is correct rather than incidental: a LoginMsg is
    // the one frame the hub sends in the clear, so the login exchange cannot
    // itself prove the session works. Only a DECRYPTED downlink can, which is
    // why the reply to the beacon carries that job.
    ASSERT_FALSE(disp.isSessionProven());

    // A fresh wake: the node beacons, the hub answers, and the answer is lost.
    disp.sendWakeBeacon(WAKE_REASON__WAKE_TIMER_CHECKIN);
    pumpUp();
    tick(800);
    const size_t answered = radio.hub_to_node_frames().size();
    ASSERT_GT(answered, 0u) << "the hub must have answered the beacon";
    delivered_ = answered;          // lost on air — D-2's scenario

    EXPECT_FALSE(disp.isSessionProven())
        << "a reply that never arrived cannot have proved anything, and it is "
           "that unproven state which arms the re-beacon ladder";
}

TEST_F(E2E, TheHubAnswersAReBeaconSoTheNodeRecoversByReAsking) {
    // D-2's recovery, end to end, and the reason no hub-side RX2 retry is
    // needed: the node re-ASKS and the hub answers again. The hub never has to
    // guess which window to re-aim at, which is better information than a
    // retry policy could have.
    bringUpSession();

    // Wake 1: answered, and the answer is lost.
    disp.sendWakeBeacon(WAKE_REASON__WAKE_TIMER_CHECKIN);
    pumpUp();
    tick(800);
    const size_t after_first = radio.hub_to_node_frames().size();
    ASSERT_GT(after_first, 0u);
    delivered_ = after_first;                 // dropped
    ASSERT_FALSE(disp.isSessionProven());

    // The ladder's re-beacon, ~4 s later. The hub answers EVERY beacon, so
    // this one gets its own reply.
    disp.sendWakeBeacon(WAKE_REASON__WAKE_TIMER_CHECKIN);
    pumpUp();
    tick(800);
    ASSERT_GT(radio.hub_to_node_frames().size(), after_first)
        << "the hub must answer the re-beacon too — that IS the retry";

    // This one lands.
    settle();
    EXPECT_TRUE(disp.isSessionProven())
        << "and the answer to the re-beacon is what recovers the session, "
           "before the 12 s REGISTER fallback ever escalates";
}
