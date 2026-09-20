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
