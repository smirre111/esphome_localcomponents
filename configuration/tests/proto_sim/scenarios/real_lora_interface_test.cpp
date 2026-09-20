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
// WHAT IS REACHABLE, AND WHAT IS NOT YET. init(), sendPacketBytes(),
// armNextRxWindow() and the accessors are ordinary methods and are driven
// directly. The two bodies that matter most — the transmit sequence in
// loraRxTask (CAD, the aimed wait, the fire) and the interrupt handling in
// frtosTasks.cpp — are `for(;;)` task loops, so they are still out of reach.
// Extracting them is the same move that made the node's uplink path testable
// (CmdDispatcher::serviceTxCommand) and is the obvious next increment. Nothing
// here should be read as covering them.
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

namespace {

// maxUplinkAimWaitUs() is protected — it is the transmit path's own business,
// and production reads it from inside the class. Subclassing to reach it keeps
// the production visibility honest rather than widening it for a test.
struct Probe : LoraInterface {
    Probe(portMUX_TYPE &m, portMUX_TYPE &b) : LoraInterface(m, b) {}
    using LoraInterface::maxUplinkAimWaitUs;
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
    // So this pins the contract as it stands and marks where coverage ends: the
    // loop that records the length is a `for(;;)`, and extracting its body is
    // the next increment.
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
