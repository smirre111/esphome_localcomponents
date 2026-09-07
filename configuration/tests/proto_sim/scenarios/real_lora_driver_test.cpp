// The real SX1278 driver, over a recording SPI bus.
//
// What these tests are about is the SHAPE of the bus traffic. The FIFO used to
// be filled one SPI transaction per byte — 61 transactions for a 60 B frame,
// 153 for a 152 B one, each taking and releasing the driver mutex — and all of
// it sits between the decision to transmit and the radio actually firing,
// which is the interval B5's prepare/fire split has to bound. Nothing above
// the driver can see that; only a fake bus can.

#include <gtest/gtest.h>

#include "spi_recorder.h"
#include "lora.h"

// Not declared in lora.h — it is the driver's own register accessor. Declared
// here because the uninitialised-read defect lived in exactly this function.
int lora_read_reg(int reg);

#include <numeric>
#include <vector>

namespace {

constexpr uint8_t kRegFifo          = 0x00;
constexpr uint8_t kRegPayloadLength = 0x22;

class DriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        spisim::bus().reset();
        lora_init();
        spisim::bus().txns.clear();   // ignore the init sequence
    }
};

std::vector<uint8_t> ramp(size_t n) {
    std::vector<uint8_t> v(n);
    std::iota(v.begin(), v.end(), (uint8_t) 1);
    return v;
}

// FIFO-write transactions only.
size_t fifoWrites() {
    size_t n = 0;
    for (const auto& t : spisim::bus().txns)
        if (t.isWrite() && t.reg() == kRegFifo) n++;
    return n;
}

}  // namespace

TEST_F(DriverTest, InitBringsUpTheBusAndAddsOneDevice) {
    EXPECT_TRUE(spisim::bus().bus_initialised);
    EXPECT_EQ(spisim::bus().devices_added, 1);
}

TEST_F(DriverTest, ASixtyByteFrameIsOneFifoTransactionNotSixtyOne) {
    const auto payload = ramp(60);
    EXPECT_EQ(lora_write(payload.data(), payload.size()), payload.size());

    EXPECT_EQ(fifoWrites(), (size_t) 1)
        << "the per-byte loop cost 60 transactions and 60 mutex acquisitions";
    EXPECT_EQ(spisim::bus().fifo, payload) << "and the bytes must be unchanged";
}

TEST_F(DriverTest, ALongFrameIsChunkedByTheBusLimitNotByTheByte) {
    // The bus is opened with dma_chan = 0, so a transaction is capped at the
    // 64-byte hardware FIFO: 63 payload bytes plus the command. 152 B is three
    // transactions — 63, 63, 26 — against 152 before.
    const auto payload = ramp(152);
    EXPECT_EQ(lora_write(payload.data(), payload.size()), payload.size());

    EXPECT_EQ(fifoWrites(), (size_t) 3);
    EXPECT_EQ(spisim::bus().fifo, payload);

    std::vector<size_t> chunk_sizes;
    for (const auto& t : spisim::bus().txns)
        if (t.isWrite() && t.reg() == kRegFifo) chunk_sizes.push_back(t.payloadLen());
    EXPECT_EQ(chunk_sizes, (std::vector<size_t>{63, 63, 26}));
}

TEST_F(DriverTest, EveryChunkCarriesTheWriteBitOnTheFifoAddress) {
    // A missing 0x80 turns a burst write into a burst read: the FIFO would be
    // untouched and the frame would go out as whatever was already there.
    const auto payload = ramp(100);
    lora_write(payload.data(), payload.size());

    size_t seen = 0;
    for (const auto& t : spisim::bus().txns) {
        if (t.reg() != kRegFifo || !t.isWrite()) continue;
        EXPECT_EQ(t.tx[0], (uint8_t) (0x80 | kRegFifo));
        seen++;
    }
    EXPECT_EQ(seen, (size_t) 2);
}

TEST_F(DriverTest, PayloadLengthIsWrittenOnceWithTheTotal) {
    const auto payload = ramp(100);
    lora_write(payload.data(), payload.size());

    EXPECT_EQ(spisim::bus().count(kRegPayloadLength, /*writes=*/true), (size_t) 1)
        << "chunking must not turn one length write into three";
    EXPECT_EQ(spisim::bus().getReg(kRegPayloadLength), (uint8_t) 100);
}

TEST_F(DriverTest, ASecondWriteAppendsRatherThanRestarting) {
    // lora_write reads REG_PAYLOAD_LENGTH and adds to it. Two calls before a
    // lora_endPacket must build one frame, not two.
    const auto a = ramp(10);
    const auto b = ramp(20);
    lora_write(a.data(), a.size());
    lora_write(b.data(), b.size());

    EXPECT_EQ(spisim::bus().getReg(kRegPayloadLength), (uint8_t) 30);
    ASSERT_EQ(spisim::bus().fifo.size(), (size_t) 30);
    EXPECT_TRUE(std::equal(a.begin(), a.end(), spisim::bus().fifo.begin()));
    EXPECT_TRUE(std::equal(b.begin(), b.end(), spisim::bus().fifo.begin() + 10));
}

TEST_F(DriverTest, AnOversizedFrameIsTruncatedNotWrapped) {
    // MAX_PKT_LENGTH is 255. A caller asking for more must get a short write
    // back, not a silently wrapped payload-length register.
    const auto payload = ramp(255);
    EXPECT_EQ(lora_write(payload.data(), payload.size()), (size_t) 255);
    EXPECT_EQ(spisim::bus().getReg(kRegPayloadLength), (uint8_t) 255);

    const auto more = ramp(10);
    EXPECT_EQ(lora_write(more.data(), more.size()), (size_t) 0);
    EXPECT_EQ(spisim::bus().fifo.size(), (size_t) 255);
}

TEST_F(DriverTest, AReadReturnsTheRegisterAndNotWhateverWasOnTheStack) {
    // lora_read_reg used to declare its RX buffer uninitialised and return
    // in[1] on BOTH paths, so a one-tick mutex timeout returned stack garbage.
    // On REG_IRQ_FLAGS that reads as an interrupt that never happened.
    //
    // The buffer is initialised now and the timeout is gone. Both halves are
    // observable here: a register the fake radio holds reads back exactly, and
    // one it does not hold reads back zero rather than varying between runs.
    spisim::bus().setReg(0x42, 0xA5);
    EXPECT_EQ(lora_read_reg(0x42), 0xA5);

    for (uint8_t r : {0x40, 0x41, 0x43, 0x44}) {
        EXPECT_EQ(lora_read_reg(r), 0) << "reg " << (int) r;
        EXPECT_EQ(lora_read_reg(r), lora_read_reg(r)) << "reg " << (int) r;
    }
}

TEST_F(DriverTest, TxIsASingleModeRegisterWrite) {
    // lora_tx() IS the fire instant. Anything else it did would be jitter in
    // front of the transmission.
    spisim::bus().txns.clear();
    lora_tx();

    ASSERT_EQ(spisim::bus().txns.size(), (size_t) 1);
    EXPECT_TRUE(spisim::bus().txns[0].isWrite());
    EXPECT_EQ(spisim::bus().txns[0].reg(), (uint8_t) 0x01);  // REG_OP_MODE
}

TEST_F(DriverTest, IdleIsASingleModeRegisterWriteToo) {
    spisim::bus().txns.clear();
    lora_idle();

    ASSERT_EQ(spisim::bus().txns.size(), (size_t) 1);
    EXPECT_TRUE(spisim::bus().txns[0].isWrite());
    EXPECT_EQ(spisim::bus().txns[0].reg(), (uint8_t) 0x01);
}
