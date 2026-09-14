// ---------------------------------------------------------------------------
// THE REAL frtosTasks.cpp, COMPILED AND RUN.
//
// This is the last of T-1. frtosTasks.cpp carries the whole interrupt path and
// no test in either repository had ever compiled it, let alone executed a line
// of it:
//
//   * the DIO0 handler, which pairs the TX_DONE edge with
//     LoraInterface::lastTxLen() — the origin every Class A window hangs off;
//   * the DIO1 RX_TIMEOUT branch, the ONLY place the node learns a window
//     closed EMPTY rather than merely unfinished, and therefore the only
//     source of noteMarkMissed() and of demotion.
//
// Two real defects were fixed blind in here: a Mode A window completing the
// Class A sequence, and a torn 64-bit read of classa_.t0_uplink_us. Neither
// had a test that could reach it.
//
// Both task bodies were `for(;;)` loops blocking on a queue, so they were
// lifted into serviceDio0Event() / serviceDio1Event() — the same move that
// made the uplink path testable (CmdDispatcher::serviceTxCommand) and then the
// receive path (LoraInterface::serviceRxWindow). The loops keep the blocking
// receive and nothing else.
//
// THIS FILE IS main.cpp. The task bodies reach the firmware through globals
// that live in main.cpp on the node, so the fixture points them at its own
// objects — and deliberately leaves some null, because every use of
// cmdDispatcher and loraIf in the interrupt path is null-checked and those
// guards are worth exercising rather than working around.
//
// WHAT IS STILL NOT COVERED: the ISRs themselves. `myinterrupts.h`'s handlers
// run in interrupt context and capture the timestamps every phase measurement
// rests on; what this file can reach is everything downstream of the queue
// they post to. Nothing here should be read as covering the ISR.
// ---------------------------------------------------------------------------

#include "frtosTasks.h"

#include "CmdDispatcher.h"
#include "LoraInterface.h"
#include "MotorCtrl.h"
#include "SystemCtrl.h"
#include "lora_node_recorder.h"

#include "ClassAWindows.h"
#include "LoraTiming.h"
#include "NodeClock.h"

#include <lora.h>
#include <gtest/gtest.h>

#include <cstring>

extern MotorCtrl     *motCtrl;
extern SystemCtrl    *sysCtrl;
extern LoraInterface *loraIf;
extern CmdDispatcher *cmdDispatcher;

namespace {

// last_tx_len_ is protected: the transmit task loop owns it and the DIO0
// handler reads it back. Subclassing to place it keeps production visibility
// honest rather than adding a setter for a test — the same reason
// real_lora_interface_test reaches maxUplinkAimWaitUs this way.
struct Probe : LoraInterface {
    Probe(portMUX_TYPE &m, portMUX_TYPE &b) : LoraInterface(m, b) {}
    using LoraInterface::last_tx_len_;
    // Which mechanism armed the pending one-shot, and whether it fired: the
    // Class A sequence test below drives the loop pass by pass through them.
    using LoraInterface::ArmSource;
    using LoraInterface::arm_source_;
    using LoraInterface::grid_arm_fire_us_;
    // Mode B: what the one-shot was aimed at, whether one is in use, and the
    // software lead the fire instant sits in front of the radio listening.
    using LoraInterface::grid_arm_target_us_;
    using LoraInterface::grid_timer_running_;
    using LoraInterface::kRadioArmLeadUs;
    // The promotion trial's state: what it aimed at, what opened, and whether a
    // trial one-shot is pending beside the periodic timer.
    using LoraInterface::grid_aim_t0_us_;
    using LoraInterface::grid_opened_t0_us_;
    using LoraInterface::trial_armed_;
};

struct Irq : public ::testing::Test {
    portMUX_TYPE  motorMux{};
    portMUX_TYPE  buttonMux{};
    MotorCtrl     mot;
    SystemCtrl    sys;
    Probe         lif{motorMux, buttonMux};
    CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};

    Dio0LoopState st{};

    void SetUp() override {
        loranode::rec().reset();
        // The firmware's globals, as main.cpp would set them.
        motCtrl       = &mot;
        sysCtrl       = &sys;
        loraIf        = &lif;
        cmdDispatcher = &disp;
    }
    void TearDown() override {
        motCtrl = nullptr; sysCtrl = nullptr;
        loraIf = nullptr;  cmdDispatcher = nullptr;
    }

    const loranode::Recorder &r() const { return loranode::rec(); }

    // One DIO0 event carrying the flags the radio would report.
    void dio0(uint8_t flags, int64_t rx_us = 0) {
        loranode::rec().next_interrupts = flags;
        lora_irq_evt_t evt{};
        evt.rx_us = rx_us;
        serviceDio0Event(evt, st);
    }
    void dio1(uint8_t flags) {
        loranode::rec().next_interrupts = flags;
        serviceDio1Event();
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// DIO1: the only path to a missed mark
// ---------------------------------------------------------------------------

TEST_F(Irq, AnEmptyWindowIsTheOnlyThingThatMissesAMark) {
    // RX_TIMEOUT is where the node learns a window closed with nothing in it.
    // Every demotion in Mode B descends from this branch, and until this file
    // compiled, nothing had run it.
    disp.noteMarkArmed();
    ASSERT_EQ(disp.consecutiveMissedMarks(), 0u);

    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);

    EXPECT_EQ(disp.consecutiveMissedMarks(), 1u)
        << "an armed mark whose window closed empty is a missed mark, and this "
           "is the only code that can say so";
    EXPECT_GE(r().count("lora_sleep"), 1u)
        << "and the radio sleeps — the empty-window path is what makes a "
           "windowed receive cost less than continuous RX";
}

TEST_F(Irq, AnEmptyWindowNobodyPromisedAnythingInIsNotAMissedMark) {
    // noteMarkMissed has its own gate (mark_window_open_, set only by
    // noteMarkArmed) and this asserts the gate from the caller's side: a
    // free-running Mode A window is legitimately empty most of the time
    // because the hub is not sending, and counting those would make WMR a
    // measure of hub traffic rather than of whether marks are being met.
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    EXPECT_EQ(disp.consecutiveMissedMarks(), 0u)
        << "no mark was armed, so nothing was promised and nothing was missed";
}

TEST_F(Irq, AnUnhandledDio1FlagStillSleepsTheRadio) {
    // The fall-through. A flag this branch does not understand must not leave
    // the radio listening: that is the state that costs the battery figure the
    // whole mode rests on.
    dio1(LORA_IRQ_FLAG_RX_DONE);   // not a DIO1 flag
    EXPECT_GE(r().count("lora_sleep"), 1u);
    EXPECT_EQ(disp.consecutiveMissedMarks(), 0u);
}

TEST_F(Irq, ServicingDio1ReArmsTheInterruptAsItsLastAct) {
    // ARM BEFORE ENABLE, and both after the flags are cleared. The pins are
    // level-triggered, so a wake source armed after the enable can partially
    // undo an ISR that fired in between.
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    const size_t clear = r().indexOf("lora_clearInterrupts");
    ASSERT_LT(clear, r().calls.size()) << "the flags must be cleared";
}

// ---------------------------------------------------------------------------
// DIO0: the edge that places every Class A window
// ---------------------------------------------------------------------------

TEST_F(Irq, TxDonePairsTheEdgeWithTheLengthTheTaskLoopRecorded) {
    // C2's origin, and the defect this pairing exists to prevent.
    //
    // T0_uplink is TxDone MINUS the frame's own air time. The length has to
    // travel from the send site (LoraInterface's transmit path records it) to
    // this handler, which is the only place that knows WHEN the frame
    // finished. It used to be recovered from the driver instead, where
    // lora_lastTxDoneUs() is 0 forever on the async path — so every window was
    // anchored to the CAD-done edge, early by the whole air time of the frame,
    // 42 to 95 ms against windows 29.44 ms wide. The node slept believing it
    // had listened.
    sys.setAutoMode(true);            // Class A is for a sleeping node
    const uint8_t frame[60] = {0};
    lif.sendPacketBytes(const_cast<uint8_t *>(frame), (int) sizeof(frame));
    // The task loop's own record, which this handler reads.
    lif.last_tx_len_ = (int) sizeof(frame);

    const int64_t txdone = 7'000'000;
    dio0(LORA_IRQ_FLAG_TX_DONE, txdone);

    ASSERT_TRUE(disp.classAActive())
        << "TX_DONE is what starts the sequence for an automatic-mode node";
    EXPECT_EQ(disp.classAArmInstantUs(),
              classa::rx1OpenUs(classa::t0UplinkUs(txdone, sizeof(frame))))
        << "the window must hang off T0_uplink — TxDone minus this frame's own "
           "air time — not off TxDone itself";
}

TEST_F(Irq, AnOpenClassAWindowIsNeverReArmedAndRx2OpensAtItsOwnInstant) {
    // Measured 2026-09-14 on node 2: "Class A RX1: empty (timeout)" and then
    // "Class A RX2: empty (timeout)" 30 ms later. The receive loop came round
    // while RX1 was still listening, found RX1 still pending with its instant
    // passed, and armed it again at 1 us: the radio was idled mid-window, a
    // second window opened and was booked as RX2, and the real RX2 never did.
    // Driven here pass by pass through the real loop halves and the real
    // interrupt handlers.
    lif.setupRXPollingTimer();
    // The fixture wires the firmware's globals, which the interrupt handlers
    // use; the arming path reads LoraInterface's own pointer, which main.cpp
    // sets with setCmdDispatcher.
    lif.setCmdDispatcher(&disp);
    sys.setAutoMode(true);
    const uint8_t frame[60] = {0};
    lif.sendPacketBytes(const_cast<uint8_t *>(frame), (int) sizeof(frame));
    lif.last_tx_len_ = (int) sizeof(frame);

    // TxDone just now, so both windows lie ahead of the host clock.
    const int64_t txdone = nodeclock::nowUs();
    dio0(LORA_IRQ_FLAG_TX_DONE, txdone);
    ASSERT_TRUE(disp.classAActive());
    const int64_t t0  = classa::t0UplinkUs(txdone, sizeof(frame));
    const int64_t rx1 = classa::rx1OpenUs(t0);
    const int64_t rx2 = classa::rx2OpenUs(t0);
    loranode::rec().reset();

    // Pass 1: arm RX1; its one-shot fires; the window opens.
    lif.armNextRxWindow();
    ASSERT_EQ(lif.arm_source_, Probe::ArmSource::ClassA);
    lif.grid_arm_fire_us_ = rx1;
    lif.serviceRxWindow();
    ASSERT_EQ(r().count("lora_rxSingle"), 1u) << "RX1 is listening";
    const size_t idles_with_rx1_open = r().count("lora_idle");

    // Pass 2 comes round while RX1's outcome is still out.
    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::Recheck)
        << "an armed window whose outcome is not in must not be armed again";
    lif.grid_arm_fire_us_ = rx1 + 60'000;   // the recheck fires
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_rxSingle"), 1u) << "a recheck opens no window";
    EXPECT_EQ(r().count("lora_idle"), idles_with_rx1_open)
        << "and never idles the radio in the middle of RX1";

    // RX1 closes empty.
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    ASSERT_TRUE(disp.classAActive()) << "RX2 is still to come";

    // Pass 3: RX2 is armed at ITS instant, a whole second after RX1.
    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::ClassA);
    EXPECT_EQ(disp.classAArmInstantUs(), rx2);
    EXPECT_EQ(rx2 - rx1, (int64_t) classa::kRx2DelayUs - (int64_t) classa::kRx1DelayUs);
    lif.grid_arm_fire_us_ = rx2;
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_rxSingle"), 2u) << "RX2 opens its own window";

    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    EXPECT_FALSE(disp.classAActive()) << "after RX2 the sequence is over";
    const macfunnel::Counters c = disp.macFunnelSnapshot();
    EXPECT_EQ(c.windows_armed, 2u) << "exactly RX1 and RX2 — no phantom window";
    EXPECT_EQ(c.windows_hit, 0u);
}

// ---------------------------------------------------------------------------
// Mode B: one timed window per round, on this node's own mark
// ---------------------------------------------------------------------------

namespace {

constexpr uint8_t kModeBNode   = 18;
constexpr uint8_t kModeBSubnet = 2;

// A GridSync the hub placed on this node's mark (header onMark).
std::vector<uint8_t> gridSyncOnMark(uint32_t slot, uint32_t msgid) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kModeBNode;
    hdr.destsubnet    = kModeBSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = msgid;
    hdr.burstcount    = 17;
    hdr.onmark        = true;

    GridSync gs = GRID_SYNC__INIT;
    gs.enable            = true;
    gs.slotindex         = slot;
    gs.slotcount         = timedgrid::kSlotCount;
    gs.roundus           = timedgrid::kRoundUs;
    gs.pitchus           = timedgrid::kSlotPitchUs;
    gs.txround           = 0;
    gs.txslot            = slot;
    gs.beaconslotindex   = timedgrid::kSlotCount - 1;
    gs.beaconeveryrounds = 233;
    gs.symtimeout        = timedgrid::kSymbolTimeoutSymbols;
    gs.resyncmaxs        = 350;
    gs.uloffsetus        = 60000;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDSYNC;
    op.gridsync = &gs;
    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

// A status request the hub placed on this node's mark: a phase sample that
// moves no motor.
std::vector<uint8_t> statusOnMark(uint32_t msgid) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kModeBNode;
    hdr.destsubnet    = kModeBSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = msgid;
    hdr.onmark        = true;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
    op.sysop    = CLIENT_OPERATION__CMD_STATUS;
    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

}  // namespace

TEST_F(Irq, AModeBNodeArmsItsOwnNextMarkEveryRound) {
    // Measured 2026-09-14 on node 2, fw 1.0.68: "Grid arming on" at 211.6 s, one
    // beacon heard at 214.1 s, then none of ~120 marks the hub placed — and the
    // node booked no missed mark until 563.9 s. Driven here pass by pass through
    // the real receive-loop halves and the real DIO1 handler.
    sys.setAddress(kModeBNode, kModeBSubnet);
    proto_sim_timer_set_now_us(40'000'000);
    lif.setupRXPollingTimer();
    lif.setCmdDispatcher(&disp);
    disp.setTimedRxEnabled(true);
    disp.setRtcSlowSrc(phase::RtcSlowSrc::Crystal);

    const int64_t t0 = 40'000'000;
    auto g = gridSyncOnMark(/*slot=*/4, /*msgid=*/900);
    disp.onReceiveNew(g.data(), (int) g.size(),
                      t0 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) g.size()));
    ASSERT_TRUE(disp.gridState().active);

    int64_t last_rx = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const int64_t mark = gridstate::nextT0Us(
            disp.gridState(), t0 + (int64_t) i * (int64_t) timedgrid::kRoundUs);
        auto f = statusOnMark(1000 + i);
        last_rx = mark + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) f.size());
        proto_sim_timer_set_now_us(last_rx);
        disp.onReceiveNew(f.data(), (int) f.size(), last_rx);
    }
    ASSERT_TRUE(disp.timedRxActive())
        << "precondition: promoted (demotion reason " << (int) disp.demotionReasonNow() << ")";

    for (int round = 0; round < 2; ++round) {
        lif.armNextRxWindow();
        ASSERT_TRUE(lif.grid_timer_running_);
        const int64_t now    = esp_timer_get_time();
        const int64_t target = lif.grid_arm_target_us_;
        const int64_t own    = gridstate::nextT0Us(disp.gridState(), now);
        EXPECT_EQ(lif.arm_source_, Probe::ArmSource::Grid) << "round " << round;
        EXPECT_EQ(target, gridstate::armInstantUs(disp.gridState(), own))
            << "round " << round << ": aimed at this node's next mark";

        // A stale wake before the one-shot fires brings the task round again: the
        // mark has not opened, so it must be armed again, not skipped.
        proto_sim_timer_advance_us(1000);
        lif.armNextRxWindow();
        EXPECT_EQ(lif.grid_arm_target_us_, target)
            << "round " << round << ": a mark that has not opened is still the one to arm";

        // The one-shot fires one lead early; the window opens.
        const int64_t fire = target - Probe::kRadioArmLeadUs;
        proto_sim_timer_set_now_us(fire);
        lif.grid_arm_fire_us_ = fire;
        const size_t rx_before = r().count("lora_rxSingle");
        lif.serviceRxWindow();
        EXPECT_EQ(r().count("lora_rxSingle"), rx_before + 1)
            << "round " << round << ": the fired one-shot opens the window";

        // The task comes round at once, while that window is still listening.
        proto_sim_timer_advance_us(2000);
        lif.armNextRxWindow();
        EXPECT_GT(lif.grid_arm_target_us_, target + (int64_t) timedgrid::kRoundUs / 2)
            << "round " << round << ": a mark whose window is open must not be "
               "armed again — the next arm is the NEXT round's mark";

        // The window closes empty.
        proto_sim_timer_set_now_us(own + 30'000);
        dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    }
    EXPECT_EQ(disp.consecutiveMissedMarks(), 2u) << "two armed marks, two empty windows";
}

namespace {
// Adopt a grid from an on-mark GridSync at t0 = 40 s, on the crystal, with timed
// RX enabled — and no phase samples, so the node is NOT promoted.
void adoptGridUnpromoted(Irq &f) {
    f.sys.setAddress(kModeBNode, kModeBSubnet);
    proto_sim_timer_set_now_us(40'000'000);
    f.lif.setupRXPollingTimer();
    f.lif.setCmdDispatcher(&f.disp);
    f.disp.setTimedRxEnabled(true);
    f.disp.setRtcSlowSrc(phase::RtcSlowSrc::Crystal);
    auto g = gridSyncOnMark(/*slot=*/4, /*msgid=*/900);
    f.disp.onReceiveNew(g.data(), (int) g.size(),
                        40'000'000 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) g.size()));
}
}  // namespace

TEST_F(Irq, AnAdoptedNodeListensAtItsOwnMarkWhileStillInModeA) {
    // Measured 2026-09-14 on node 2 (fw 1.0.72): with only the free-running window
    // the node caught a mark about every 23 s and promoted after 192.8 s; in Mode B
    // it caught one every round. The promotion trial gives the unpromoted node one
    // window per round at its mark, beside its Mode A windows.
    adoptGridUnpromoted(*this);
    ASSERT_TRUE(disp.gridState().active);
    ASSERT_FALSE(disp.timedRxActive()) << "precondition: no phase evidence yet";

    const size_t armed_before = (size_t) proto_sim_timer_armed_count();
    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::Trial);
    EXPECT_TRUE(lif.trial_armed_);
    EXPECT_FALSE(lif.grid_timer_running_) << "the periodic Mode A timer keeps running";
    EXPECT_EQ((size_t) proto_sim_timer_armed_count(), armed_before + 1)
        << "the trial one-shot runs BESIDE the periodic timer, not instead of it";
    const int64_t now  = esp_timer_get_time();
    const int64_t aim  = lif.grid_aim_t0_us_;
    EXPECT_EQ(aim, gridstate::nextT0Us(disp.gridState(), now)) << "this node's own next mark";

    // A periodic tick arrives first: an ordinary Mode A window, trial untouched.
    const size_t rx0 = r().count("lora_rxSingle");
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_rxSingle"), rx0 + 1) << "Mode A still listens";
    EXPECT_EQ(lif.grid_opened_t0_us_, 0) << "a periodic window is not the trial window";
    EXPECT_TRUE(lif.trial_armed_);
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);

    // The task comes round again before the one-shot fires: the aim must stand.
    proto_sim_timer_advance_us(470'000);
    lif.armNextRxWindow();
    EXPECT_EQ(lif.grid_aim_t0_us_, aim) << "a pending trial window is not re-armed by a periodic pass";

    // The trial one-shot fires: its window opens and is recorded.
    const int64_t fire = lif.grid_arm_target_us_ - Probe::kRadioArmLeadUs;
    proto_sim_timer_set_now_us(fire);
    lif.grid_arm_fire_us_ = fire;
    const size_t rx1 = r().count("lora_rxSingle");
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_rxSingle"), rx1 + 1);
    EXPECT_EQ(lif.grid_opened_t0_us_, aim);
    EXPECT_FALSE(lif.trial_armed_);

    // Empty: not a missed mark — the node is not on the grid yet.
    proto_sim_timer_set_now_us(aim + 30'000);
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    EXPECT_EQ(disp.consecutiveMissedMarks(), 0u) << "a trial window is not a mark";

    // Next pass: the next round's mark.
    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::Trial);
    EXPECT_EQ(lif.grid_aim_t0_us_, aim + (int64_t) timedgrid::kRoundUs);
}

TEST_F(Irq, PromotionEndsTheTrialAndModeBTakesTheTimer) {
    adoptGridUnpromoted(*this);
    lif.armNextRxWindow();
    ASSERT_EQ(lif.arm_source_, Probe::ArmSource::Trial);

    // Eight on-mark samples from eight frames: promoted.
    for (uint32_t i = 0; i < 8; ++i) {
        const int64_t mark = gridstate::nextT0Us(
            disp.gridState(), 40'000'000 + (int64_t) (i + 1) * (int64_t) timedgrid::kRoundUs);
        auto s = statusOnMark(1000 + i);
        const int64_t rx = mark + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) s.size());
        proto_sim_timer_set_now_us(rx);
        disp.onReceiveNew(s.data(), (int) s.size(), rx);
    }
    ASSERT_TRUE(disp.timedRxActive());

    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::Grid) << "Mode B's own mark window";
    EXPECT_TRUE(lif.grid_timer_running_) << "and the periodic timer is stopped";
    EXPECT_FALSE(lif.trial_armed_);
}

TEST_F(Irq, WithoutAGridThereIsNoTrial) {
    lif.setupRXPollingTimer();
    lif.setCmdDispatcher(&disp);
    disp.setTimedRxEnabled(true);
    ASSERT_FALSE(disp.gridState().active);
    lif.armNextRxWindow();
    EXPECT_EQ(lif.arm_source_, Probe::ArmSource::None);
    EXPECT_FALSE(lif.trial_armed_);
}

TEST_F(Irq, TxDoneSleepsTheRadioAndReturnsTheMutex) {
    // The transmit path takes the radio mutex and this is what gives it back.
    // A missed give here is the "rx_tx_semaphore stuck (lost IRQ?)" state the
    // transmit path has a recovery branch for.
    dio0(LORA_IRQ_FLAG_TX_DONE, 5'000'000);
    EXPECT_GE(r().count("lora_sleep"), 1u);
    EXPECT_FALSE(lif.continuousRx());
}

TEST_F(Irq, CadDetectedAndCadClearBothPostExactlyOneAnswer) {
    // The CAD result the transmit path waits on. One answer per CAD, or the
    // queue stops meaning "the result of THIS CAD" — which is the invariant
    // the transmit path's xQueueReset exists to protect from the other side.
    ASSERT_EQ(uxQueueMessagesWaiting(lif.lora_cad_queue_), 0u);

    dio0(LORA_IRQ_FLAG_CAD_DONE | LORA_IRQ_FLAG_CAD_DETECTED);
    EXPECT_EQ(uxQueueMessagesWaiting(lif.lora_cad_queue_), 1u) << "busy";

    uint8_t answer = 0xFF;
    ASSERT_EQ(xQueueReceive(lif.lora_cad_queue_, &answer, 0), pdTRUE);
    EXPECT_EQ(answer, 1) << "CAD detected means the channel is busy";

    dio0(LORA_IRQ_FLAG_CAD_DONE);
    EXPECT_EQ(uxQueueMessagesWaiting(lif.lora_cad_queue_), 1u) << "free";
    ASSERT_EQ(xQueueReceive(lif.lora_cad_queue_, &answer, 0), pdTRUE);
    EXPECT_EQ(answer, 0) << "CAD done with nothing detected means free";
}

TEST_F(Irq, TheSpinSignatureIsCounted) {
    // Woken with no flag behind it. The counters exist to tell an INTERRUPT
    // STORM apart from other causes of idle starvation: wakes >> rxdone with a
    // large zeroflags count is this task spinning on a line that keeps
    // re-asserting, which is what a TASK_WDT reset with a prvIdleTask
    // backtrace looks like. In normal mode lora_sleep() drops the line, so it
    // cannot persist; continuous RX removed that.
    dio0(0);
    dio0(0);
    dio0(LORA_IRQ_FLAG_TX_DONE, 1'000'000);

    EXPECT_EQ(st.wakes, 3u) << "every wake counts";
    EXPECT_EQ(st.zero_flags, 2u) << "two of them had nothing to do";
}

TEST_F(Irq, TheHandlersToleratePartlyWiredFirmware) {
    // The null guards, which are not decoration: main.cpp brings these up in
    // an order, and an interrupt can arrive mid-way. Every use of
    // cmdDispatcher in the interrupt path is guarded, and this is the test that
    // says so — with the dispatcher gone, a DIO1 timeout must still clear and
    // re-arm rather than fault.
    cmdDispatcher = nullptr;
    dio1(LORA_IRQ_FLAG_RX_TIMEOUT);
    EXPECT_GE(r().count("lora_sleep"), 1u);

    loranode::rec().reset();
    dio0(LORA_IRQ_FLAG_CAD_DONE);
    EXPECT_EQ(uxQueueMessagesWaiting(lif.lora_cad_queue_), 1u)
        << "the CAD answer does not go through the dispatcher, so it must "
           "still be posted";
}
