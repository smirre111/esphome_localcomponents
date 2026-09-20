// ---------------------------------------------------------------------------
// THE REAL LoraInterface.cpp, COMPILED.
//
// §11b's last structural entry: "LoraInterface.cpp and frtosTasks.cpp are not
// compiled by the host suite at all (the CMake shims LoraInterface.h)". Two
// defects this year were invisible for exactly that reason — the uplink-aim
// call site went in untested, and a log line sitting inside the aimed critical
// path (3.5 ms of UART at 115200 baud, a quarter of the guard band) was caught
// by reading the diff rather than by any test. Every fix in that file was made
// blind.
//
// This target compiles it against the REAL driver header
// (components/lora/include/lora.h) with the driver IMPLEMENTATION replaced by a
// recorder — the arrangement the hub has had since real_lora_tracker landed.
// Using the real header rather than a paraphrase is the point: a signature that
// drifts is a build error here instead of a surprise on the roof.
//
// WHAT IS REACHABLE, AND WHAT IS NOT. init(), sendPacketBytes(),
// armNextRxWindow() and the accessors are ordinary methods and are driven
// directly.
//
// The receive task's body is reachable TOO, as of the extraction this file
// drove: serviceRxWindow / armTimedRxWindow / armContinuousRx /
// noteRxWindowSkipped / transmitOneQueuedFrame / beginCad were lifted out of
// loraRxTask's `for(;;)` without moving or reordering anything, exactly as
// CmdDispatcher::serviceTxCommand was lifted out of its own task loop. What
// the loop still owns is the blocking and the watchdog — two semaphore takes
// and a queue receive — and that is all it owns.
//
// STILL OUT OF REACH: frtosTasks.cpp, which is not compiled at all (it needs
// an ADC shim and the motor/battery task surface), and therefore the whole
// interrupt path — the DIO0/DIO1 handler, the TX_DONE branch that pairs with
// last_tx_len_, and the ISR timestamps every phase measurement rests on.
//
// AND ONE HONEST LIMIT ON WHAT IS REACHED. A test here can assert the ORDER
// of the radio operations and which branch ran; it cannot assert how long the
// critical path took. The log line that sat between the aimed instant and the
// CAD would still not be caught by anything below — what catches that is the
// comment now standing in its place, and a scope. Do not read this file as
// covering the aim's timing.
//
// The win already banked: THE PHY IS PINNED. Every constant in LoraTiming.h is
// derived from the spreading factor, the bandwidth, the coding rate and the
// CRC flag, and until this file compiled, nothing in either repository checked
// that the radio is programmed to the PHY the arithmetic assumes.
// ---------------------------------------------------------------------------

#include "LoraInterface.h"
#include "lora_node_recorder.h"

#include "LoraTiming.h"
#include "TimedGrid.h"

#include <lora.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

// maxUplinkAimWaitUs() is protected — it is the transmit path's own business,
// and production reads it from inside the class. Subclassing to reach it keeps
// the production visibility honest rather than widening it for a test.
struct Probe : LoraInterface {
    Probe(portMUX_TYPE &m, portMUX_TYPE &b) : LoraInterface(m, b) {}
    using LoraInterface::maxUplinkAimWaitUs;
    // ArmSource decides whether an opened window counts as a MARK, which is
    // what WMR is computed from and therefore what demotion turns on. It is
    // protected because only the arming path may set it; a test has to be able
    // to say which mechanism armed this window.
    using LoraInterface::ArmSource;
    using LoraInterface::arm_source_;
    using LoraInterface::cont_rx_armed_;
    using LoraInterface::continuous_rx_;
};

struct Iface : public ::testing::Test {
    portMUX_TYPE  motorMux{};
    portMUX_TYPE  buttonMux{};
    Probe         lif{motorMux, buttonMux};

    void SetUp() override { loranode::rec().reset(); }

    const loranode::Recorder &r() const { return loranode::rec(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// The PHY the whole design is derived from
// ---------------------------------------------------------------------------

TEST_F(Iface, InitProgramsThePhyLoraTimingAssumes) {
    // T_sym = 2^SF / BW, and every window, guard and symbol count in the design
    // falls out of it. If the radio is set to a different SF or bandwidth than
    // LoraTiming.h believes, every instant in the system is wrong by a ratio —
    // and nothing would have said so.
    lif.init();

    EXPECT_EQ(r().sf, (int) loratiming::kSpreadingFactor);
    EXPECT_EQ(r().bw, (long) loratiming::kBandwidthHz);
    EXPECT_EQ(r().cr_denom, (int) loratiming::kCodingRateDenom);
    EXPECT_EQ(r().preamble_len, (long) loratiming::kPreambleSymbols);
    EXPECT_EQ(r().crc_on, loratiming::kCrcOn)
        << "the CRC contributes 16 bits to the symbol count; a radio with it "
           "off makes every frame shorter than the arithmetic says";
}

TEST_F(Iface, TheProgrammedPhyReproducesTheDesignsOwnSymbolTime) {
    // Not a restatement of the constants: the check is that the numbers the
    // radio was GIVEN, put back through the design's own formula, produce the
    // 256 us symbol the static_asserts pin.
    lif.init();
    ASSERT_GT(r().bw, 0);

    const uint32_t t_sym_us =
        (uint32_t) ((1ull << r().sf) * 1000000ull / (uint64_t) r().bw);
    EXPECT_EQ(t_sym_us, loratiming::kSymbolUs);
    EXPECT_EQ(t_sym_us, 256u);
}

TEST_F(Iface, InitLeavesTheRadioAsleepWithInterruptsCleared) {
    // The state the RX path expects to start from. A radio left in continuous
    // receive here would burn the battery figure the whole mode rests on, and
    // stale interrupt flags are read as an event that already happened.
    lif.init();

    EXPECT_EQ(r().count("lora_clearInterrupts"), 1u);
    EXPECT_GE(r().count("lora_sleep"), 1u);
    EXPECT_GT(r().indexOf("lora_sleep"), r().indexOf("lora_clearInterrupts"))
        << "clear first, then sleep: sleeping on stale flags leaves an event "
           "waiting that never happened";
}

TEST_F(Iface, InitMapsDio1ToRxTimeoutWhichIsWhatEndsAWindow) {
    // DIO1 is how the node learns a window closed EMPTY — the only place
    // noteMarkMissed comes from, and therefore the only path to demotion. If
    // DIO1 were mapped to anything else, a node would never demote and its own
    // diagnostics would report a healthy link.
    lif.init();
    EXPECT_EQ(r().dio_mode[1], (uint8_t) LORA_IRQ_DIO1_RXTIMEOUT);
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

TEST_F(Iface, SendPacketBytesPutsExactlyThoseBytesOnTheAir) {
    // The FIFO fill was once one SPI transaction PER BYTE, each with its own
    // mutex acquisition, sitting between the decision to send and the radio
    // firing. What a test can hold it to is that the bytes arrive intact and
    // that a frame is actually closed.
    const uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    lif.sendPacketBytes(const_cast<uint8_t *>(frame), (int) sizeof(frame));

    ASSERT_EQ(r().packets.size(), 1u) << "one frame, closed";
    EXPECT_EQ(r().packets[0],
              std::vector<uint8_t>(frame, frame + sizeof(frame)));
}

TEST_F(Iface, TheTransmitLengthIsTheTaskLoopsToRecordNotThisMethods) {
    // A BOUNDARY MARKER, and the honest shape of what this target reaches so
    // far. My first draft of this test asserted lastTxLen() after
    // sendPacketBytes() and failed — correctly, because the method does not set
    // it. The task loop does, at the call site (`this->last_tx_len_ =
    // len_sent`).
    //
    // That split is deliberate and load-bearing. C2 places the node's receive
    // windows from T0_uplink = t_txdone - n_sym(len)*T_sym, so the LENGTH has
    // to travel from the send site to the TX_DONE handler. It used to be
    // recovered from the driver instead, where lora_lastTxDoneUs() is 0 forever
    // on the async path — so the windows were anchored to the CAD-done edge,
    // early by the frame's whole air time, 42 to 95 ms against a 29.44 ms
    // window. The node slept believing it had listened.
    //
    // So this pins the contract as it stands and marks where coverage ends.
    // The loop that records the length is no longer the obstacle — its body is
    // transmitOneQueuedFrame() now, and the tests below drive it — but the
    // TX_DONE handler that READS the length lives in frtosTasks.cpp, which is
    // still not compiled. The pairing is therefore still unverified end to end.
    const uint8_t frame[25] = {0};
    lif.sendPacketBytes(const_cast<uint8_t *>(frame), (int) sizeof(frame));

    ASSERT_EQ(r().packets.size(), 1u) << "the frame did go out";
    EXPECT_EQ(lif.lastTxLen(), 0)
        << "sendPacketBytes must NOT claim to have recorded the length — the "
           "task loop owns that, and a method that set it here would hide which "
           "of the two the TX_DONE handler is actually reading";
}

// ---------------------------------------------------------------------------
// The aim bound, now that the file it lives in is compiled
// ---------------------------------------------------------------------------

TEST_F(Iface, TheUplinkAimBoundIsTheBackoffItReplaces) {
    // maxUplinkAimWaitUs() is the bound the aimed uplink may wait for its mark,
    // and the only honest value is the delay the waiting REPLACES. It is
    // derived from the same member the random backoff uses so the two cannot
    // drift — and this is the first test able to read it at all, because the
    // header it lives in was shimmed away until now.
    //
    // The backoff is slotDurationMs + slotDurationMs * (esp_random() % 10), so
    // its worst case is ten slots.
    EXPECT_EQ(lif.maxUplinkAimWaitUs(),
              (int64_t) lif.rxWindowPeriodMs() * 0 + 290000)
        << "29 ms x 10, from roundDurationMs / (rxSlotsPerRound * "
           "txSlotsPerRound)";
    EXPECT_LT(lif.maxUplinkAimWaitUs(), (int64_t) timedgrid::kRoundUs)
        << "and below one round, so an aimed frame can never sit waiting for a "
           "later round";
}

// ---------------------------------------------------------------------------
// The receive task's body, now that it is callable
//
// Each test below drives one of the methods lifted out of loraRxTask. What
// they are worth is that until this extraction, NOTHING checked that a window
// is opened with the right six register writes in the right order, or which of
// the transmit retry paths returns the buffer to the pool — and both have a
// failure mode that is silent on both ends of the link.
// ---------------------------------------------------------------------------

TEST_F(Iface, ATimedWindowIsOpenedWithTheSixWritesInOrder) {
    // A window is not "the radio is listening"; it is this exact sequence. Get
    // the order wrong and the failure is silent: arming before the DIO map is
    // written means the first edge is reported against the previous mapping,
    // and clearing interrupts AFTER rxSingle discards the event the window was
    // opened for.
    lif.armTimedRxWindow();

    const size_t idle  = r().indexOf("lora_idle");
    const size_t dio   = r().indexOf("lora_setInterruptMode");
    const size_t symto = r().indexOf("lora_setSymbolTimeout");
    const size_t clr   = r().indexOf("lora_clearInterrupts");
    const size_t single= r().indexOf("lora_rxSingle");

    ASSERT_LT(single, r().calls.size()) << "the window must actually open";
    EXPECT_LT(idle, dio)    << "idle before remapping DIO";
    EXPECT_LT(dio, symto);
    EXPECT_LT(symto, clr);
    EXPECT_LT(clr, single)
        << "clear the flags BEFORE listening, or the window's own first edge "
           "is thrown away with the stale ones";

    EXPECT_EQ(r().dio_mode[0], (uint8_t) LORA_IRQ_DIO0_RXDONE)
        << "DIO0 must report RX_DONE for a receive window — left on CADDONE "
           "from a transmit, the window cannot hear a frame at all";
    EXPECT_EQ(r().dio_mode[1], (uint8_t) LORA_IRQ_DIO1_RXTIMEOUT);
}

TEST_F(Iface, TheWindowIsOpenedExactlyAsWideAsTheGuardBandAssumes) {
    // The one radio setting Mode B's geometry depends on. G = (W - T_detect)/2
    // is computed from kSymbolTimeoutSymbols in TimedGrid.h, so if the register
    // is written with anything else, every guard band in the design is wrong by
    // half the difference — and a node would look like it was merely unlucky.
    lif.armTimedRxWindow();

    EXPECT_EQ(r().sym_timeout, (uint16_t) timedgrid::kSymbolTimeoutSymbols);
    // And the width that implies, back through the design's own symbol time.
    EXPECT_EQ((uint32_t) r().sym_timeout * loratiming::kSymbolUs,
              timedgrid::kWindowUs);
    // And therefore the guard band, which is the number the whole timed mode
    // is judged by: G = (W - T_detect) / 2.
    EXPECT_EQ(timedgrid::kGuardUs,
              ((uint32_t) r().sym_timeout * loratiming::kSymbolUs -
               timedgrid::kDetectUs) / 2);
}

TEST_F(Iface, ContinuousRxIsArmedOnceAndNotReArmedEveryPass) {
    // The drift test's whole point is to stop missing burst copies. The
    // semaphore that drives this fires every ~500 ms, and an idle + re-arm on
    // each pass would drop the radio out of receive each time — reopening the
    // gap the mode exists to close, while the logs showed it enabled.
    lif.continuous_rx_ = true;

    lif.armContinuousRx();
    const size_t receives_after_first = r().count("lora_receive");
    ASSERT_EQ(receives_after_first, 1u);
    EXPECT_TRUE(lif.cont_rx_armed_);

    lif.armContinuousRx();
    lif.armContinuousRx();
    EXPECT_EQ(r().count("lora_receive"), 1u)
        << "re-arming would take the radio out of RX for the duration of the "
           "idle, which is exactly the loss continuous RX is meant to prevent";
    EXPECT_EQ(r().count("lora_idle"), 0u);
}

TEST_F(Iface, NoteRadioSleptForcesAReArmSoTheNodeCannotGoDeaf) {
    // v1.0.33: any other path that slept the radio left cont_rx_armed_ still
    // claiming it was armed, and the node was deaf until reboot. The flag has
    // to be resettable from outside the arming path for that reason.
    lif.continuous_rx_ = true;
    lif.armContinuousRx();
    ASSERT_EQ(r().count("lora_receive"), 1u);

    lif.noteRadioSlept();
    lif.armContinuousRx();
    EXPECT_EQ(r().count("lora_receive"), 2u)
        << "after something else slept the radio, the next pass must re-arm";
}

TEST_F(Iface, ATimedWindowLeavesContinuousRxNeedingAReArm) {
    // Leaving the drift test has to re-arm properly on the same pass, so the
    // windowed path clears the flag rather than assuming it was already false.
    lif.cont_rx_armed_ = true;
    lif.armTimedRxWindow();
    EXPECT_FALSE(lif.cont_rx_armed_);
}

TEST_F(Iface, ASkippedWindowIsCountedAndTouchesNoRadioRegister) {
    // The radio was busy for the whole 700 ms, so this window never opened.
    // Two things must be true, and the second is the subtle one: the skip is
    // COUNTED (an unarmed window is invisible to WMR by construction, because
    // noteMarkArmed is what opens a mark), and the radio is left completely
    // alone — a half-armed window here would be worse than no window, since it
    // would interrupt whatever was actually using the radio.
    EXPECT_EQ(lif.rxBusySkips(), 0u);

    lif.noteRxWindowSkipped();
    lif.noteRxWindowSkipped();

    EXPECT_EQ(lif.rxBusySkips(), 2u);
    EXPECT_TRUE(r().calls.empty())
        << "a window that never opened must not have touched the radio";
}

// ---------------------------------------------------------------------------
// The transmit body
// ---------------------------------------------------------------------------

namespace {

// Push one frame into the transmit queue the way production does — a pooled
// buffer, by pointer — and hand back what the pool had free beforehand so a
// test can prove the buffer came back.
struct Queued {
    LoraInterface::rx_buffer_t *buf;
    UBaseType_t                 free_before;
};

Queued queueOneFrame(Probe &lif, const uint8_t *bytes, int len) {
    const UBaseType_t free_before =
        uxQueueMessagesWaiting(lif.tx_memory_pool.free_buffer_queue);
    LoraInterface::rx_buffer_t *buf = lif.get_free_tx_buffer(0);
    EXPECT_NE(buf, nullptr);
    if (buf) {
        memcpy(buf->data, bytes, (size_t) len);
        buf->length = len;
        EXPECT_EQ(xQueueSend(lif.tx_memory_pool.data_queue, &buf, 0), pdTRUE);
    }
    return Queued{buf, free_before};
}

// Answer every CAD with `busy`, from inside lora_cad() — where the real
// CAD-DONE interrupt posts it, and the only place production has not just
// wiped the queue. NOTE the type: lora_cad_queue_ is created with a ONE-BYTE
// item size while the consumer reads into an int, so the answer must be a
// uint8_t. That mismatch is pre-existing and works only because the ESP32 is
// little-endian and the int's upper bytes are zero; writing an int here would
// queue three bytes of nothing.
void answerCadWith(Probe &lif, uint8_t detected) {
    loranode::rec().on_cad = [&lif, detected]() {
        uint8_t v = detected;
        xQueueSend(lif.lora_cad_queue_, &v, 0);
    };
}

}  // namespace

TEST_F(Iface, AnEmptyTransmitQueueIsNotATransmit) {
    EXPECT_FALSE(lif.transmitOneQueuedFrame());
    EXPECT_TRUE(r().calls.empty())
        << "nothing queued must mean the radio is never touched — this runs "
           "every pass of the receive task";
}

TEST_F(Iface, AClearChannelSendsTheFrameAndReturnsTheBuffer) {
    const uint8_t frame[] = {0x11, 0x22, 0x33, 0x44};
    const Queued q = queueOneFrame(lif, frame, (int) sizeof(frame));
    answerCadWith(lif, 0 /* channel free */);

    EXPECT_TRUE(lif.transmitOneQueuedFrame());

    ASSERT_EQ(r().packets.size(), 1u);
    EXPECT_EQ(r().packets[0], std::vector<uint8_t>(frame, frame + sizeof(frame)));
    EXPECT_EQ(r().count("lora_cad"), 1u) << "one CAD, one send";
    EXPECT_EQ(lif.lastTxLen(), (int) sizeof(frame))
        << "the length the TX_DONE handler recovers T0_uplink from is recorded "
           "HERE, at the call site, not by sendPacketBytes";
    EXPECT_EQ(uxQueueMessagesWaiting(lif.tx_memory_pool.free_buffer_queue),
              q.free_before);
}

TEST_F(Iface, ABusyChannelSendsNothingAndStillReturnsTheBuffer) {
    // The leak this asserts was real: the buffer used to be returned only on
    // the success path, so sustained channel contention starved the five-entry
    // TX pool until a reboot. Nothing could reach the code to say so.
    const uint8_t frame[] = {0xAA, 0xBB};
    const Queued q = queueOneFrame(lif, frame, (int) sizeof(frame));
    answerCadWith(lif, 1 /* channel busy, every time */);

    EXPECT_TRUE(lif.transmitOneQueuedFrame())
        << "a frame WAS dequeued — the return value says that, not that it was "
           "sent, so an exhausted retry loop cannot read as an empty queue";

    EXPECT_TRUE(r().packets.empty()) << "CAD is the collision backstop";
    EXPECT_EQ(r().count("lora_cad"), 5u) << "max_retries";
    EXPECT_EQ(lif.lastTxLen(), 0) << "nothing went out, so no length to carry";
    EXPECT_EQ(uxQueueMessagesWaiting(lif.tx_memory_pool.free_buffer_queue),
              q.free_before)
        << "the buffer must come back on the exhausted path too, or the pool "
           "drains one frame at a time under contention";
}

TEST_F(Iface, NoCadAnswerAtAllAbandonsTheFrameWithoutSendingIt) {
    // F-32: the receive is bounded at 2 s so a radio that never reports CAD
    // done cannot hold the radio semaphore forever. On timeout the retry is
    // counted and the semaphore released; after max_retries the frame is
    // abandoned. What must NOT happen is a send on no answer.
    const uint8_t frame[] = {0x01};
    const Queued q = queueOneFrame(lif, frame, (int) sizeof(frame));
    loranode::rec().on_cad = nullptr;   // the radio says nothing

    EXPECT_TRUE(lif.transmitOneQueuedFrame());

    EXPECT_TRUE(r().packets.empty());
    EXPECT_EQ(r().count("lora_cad"), 5u);
    EXPECT_EQ(uxQueueMessagesWaiting(lif.tx_memory_pool.free_buffer_queue),
              q.free_before);
}

TEST_F(Iface, AStaleCadAnswerIsNotConsumedAsThisCadsAnswer) {
    // THE REGRESSION THIS EXTRACTION WAS WORTH DOING FOR.
    //
    // The CAD receive abandons its answer after 2 s; that answer stays queued.
    // If the queue is not reset immediately before lora_cad(), the next CAD
    // consumes it — a "channel free" from a second ago, decided when the
    // channel was in fact free, applied to a moment when it is not. The node
    // then transmits into a running 17-copy burst, which is the exact collision
    // CAD exists to prevent, and both ends log success.
    //
    // So: leave a stale FREE in the queue, and have the radio report BUSY for
    // this CAD. With the reset, the stale answer is gone and nothing goes out.
    // Delete the xQueueReset in beginCad() and this test transmits.
    const uint8_t frame[] = {0x77, 0x88};
    const Queued q = queueOneFrame(lif, frame, (int) sizeof(frame));

    uint8_t stale_free = 0;
    ASSERT_EQ(xQueueSend(lif.lora_cad_queue_, &stale_free, 0), pdTRUE);
    answerCadWith(lif, 1 /* the channel is busy NOW */);

    EXPECT_TRUE(lif.transmitOneQueuedFrame());

    EXPECT_TRUE(r().packets.empty())
        << "a stale 'channel free' was consumed as this CAD's answer — the "
           "node just transmitted into a burst";
    EXPECT_EQ(uxQueueMessagesWaiting(lif.tx_memory_pool.free_buffer_queue),
              q.free_before);
}

TEST_F(Iface, TheCadSetupHappensInAnOrderThatMakesTheAnswerThisCads) {
    // beginCad's whole job, stated as a sequence: leave whatever mode the radio
    // was in, point DIO0 at CAD_DONE, THEN drop any older answer, and only then
    // start the CAD. A reset placed after lora_cad() would race the interrupt
    // and discard the real answer; a reset before the DIO remap would leave a
    // window's RX_DONE to be read as a CAD result.
    lif.beginCad();

    const size_t idle = r().indexOf("lora_idle");
    const size_t dio  = r().indexOf("lora_setInterruptMode");
    const size_t cad  = r().indexOf("lora_cad");

    ASSERT_LT(cad, r().calls.size());
    EXPECT_LT(idle, dio);
    EXPECT_LT(dio, cad);
    EXPECT_EQ(r().dio_mode[0], (uint8_t) LORA_IRQ_DIO0_CADDONE);
    EXPECT_EQ(r().dio_mode[1], (uint8_t) LORA_IRQ_DIO1_RXTIMEOUT);

    // The reset is not a recorded radio call — it is a queue operation — so
    // what a test can hold it to is the observable consequence, which is the
    // previous test. This one pins the register order the reset sits inside.
    EXPECT_EQ(uxQueueMessagesWaiting(lif.lora_cad_queue_), 0u)
        << "no answer was posted, and none was left over";
}

TEST_F(Iface, ServicingAWindowDispatchesOnTheDriftTestFlag) {
    // serviceRxWindow is the loop's one call for the receive half: take the
    // radio mutex, then open the kind of window the mode asks for. The busy
    // branch is unreachable through it on a harness whose xSemaphoreTake always
    // succeeds — which is exactly why noteRxWindowSkipped is callable alone.
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_rxSingle"), 1u);
    EXPECT_EQ(r().count("lora_receive"), 0u);
    EXPECT_EQ(lif.rxBusySkips(), 0u);

    loranode::rec().reset();
    lif.continuous_rx_ = true;
    lif.serviceRxWindow();
    EXPECT_EQ(r().count("lora_receive"), 1u);
    EXPECT_EQ(r().count("lora_rxSingle"), 0u);
}

// ---------------------------------------------------------------------------
// U-2: the radio-busy skip, and what a failed transmit does to a drift test
// ---------------------------------------------------------------------------

TEST_F(Iface, AFailedTransmitDoesNotLeaveTheNodeDeafInADriftTest) {
    // v1.0.33 again, through a door noteRadioSlept() did not cover.
    //
    // cont_rx_armed_ means "the radio is already sitting in continuous RX, so
    // do not idle and re-arm it every ~500 ms". Every path that SLEEPS the
    // radio clears it (frtosTasks, five call sites). But the transmit path does
    // not sleep the radio, it IDLES it — in beginCad(), and again in the
    // stuck-mutex recovery — and idling stops reception just as dead.
    //
    // On the success path that self-heals: TX_DONE sleeps the radio and clears
    // the flag. The FAILING paths have no TX_DONE. So a transmit whose CAD
    // never comes back clear leaves the radio in standby with the flag still
    // claiming it is armed, and armContinuousRx() then does nothing on every
    // subsequent pass — the node is deaf for the rest of the drift test, which
    // is the one measurement session whose numbers have to be trustworthy.
    lif.continuous_rx_ = true;
    lif.armContinuousRx();
    ASSERT_TRUE(lif.cont_rx_armed_);
    ASSERT_EQ(r().count("lora_receive"), 1u);

    const uint8_t frame[] = {0x5A};
    queueOneFrame(lif, frame, (int) sizeof(frame));
    answerCadWith(lif, 1 /* busy every time, so every retry is exhausted */);
    lif.transmitOneQueuedFrame();

    ASSERT_TRUE(r().packets.empty()) << "nothing went out, as set up";
    ASSERT_GT(r().count("lora_idle"), 0u) << "but the radio WAS idled";
    EXPECT_FALSE(lif.cont_rx_armed_)
        << "the radio was idled out of continuous RX, so the flag must not "
           "still claim it is armed — the next pass has to re-arm or the node "
           "stays deaf for the rest of the drift test";

    // And the next pass must actually re-arm it.
    lif.armContinuousRx();
    EXPECT_EQ(r().count("lora_receive"), 2u);
}

TEST_F(Iface, ABusyRadioIsNotASkippedWindowWhileTheDriftTestHoldsIt) {
    // The skip counter answers "a window the node meant to open never opened",
    // which is the one Mode B failure the KPIs cannot see: an unarmed window is
    // invisible to WMR by construction, because noteMarkArmed is what opens a
    // mark.
    //
    // During a drift test the radio is deliberately held in continuous RX, and
    // the mutex is released only by an interrupt — so most passes find it busy.
    // Counting those makes the number say "this node is failing to open
    // windows" throughout a bench run that is working exactly as designed, and
    // the count is reported to the hub. A diagnostic that cries wolf for the
    // duration of a measurement is worse than no diagnostic.
    lif.continuous_rx_ = true;

    lif.noteRxWindowSkipped();
    lif.noteRxWindowSkipped();
    EXPECT_EQ(lif.rxBusySkips(), 0u)
        << "the radio being busy IS the drift test — not a failure to arm";

    lif.continuous_rx_ = false;
    lif.noteRxWindowSkipped();
    EXPECT_EQ(lif.rxBusySkips(), 1u) << "and in normal mode it still counts";
}

TEST_F(Iface, TheSkipWarningIsThrottledButTheCountIsNot) {
    // The log line fires per PASS — every rxIntervalMs, so about twice a second
    // — and whatever wedges the radio mutex wedges it for a while. Unthrottled
    // it buried every other line the node emits while the interesting event
    // was scrolling past, and at 115200 baud each line costs ~3.5 ms of UART,
    // which is a term in the very arm residual this path exists to diagnose.
    //
    // The COUNT must stay exact regardless: it is the payload, and it is what
    // travels on PhaseReport.
    ::testing::internal::CaptureStderr();
    for (int i = 0; i < 40; ++i) lif.noteRxWindowSkipped();
    const std::string logged = ::testing::internal::GetCapturedStderr();

    EXPECT_EQ(lif.rxBusySkips(), 40u) << "every skip counts";

    size_t lines = 0;
    for (size_t at = logged.find("RX window skipped");
         at != std::string::npos;
         at = logged.find("RX window skipped", at + 1))
        ++lines;
    EXPECT_EQ(lines, 4u)
        << "first three, then every 32nd — not forty lines of the same fact";
}
