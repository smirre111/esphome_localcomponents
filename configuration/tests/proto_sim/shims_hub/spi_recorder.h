// Records what the SX1278 driver actually puts on the SPI bus.
//
// The point is the SHAPE of the traffic, not its content: how many
// transactions a FIFO fill takes, in what order, with which command bytes.
// That is the thing the per-byte FIFO loop got wrong and the thing B5's
// prepare/fire split has to bound, and it is invisible from above the driver.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace spisim {

struct Txn {
    std::vector<uint8_t> tx;   // bytes clocked out, command byte first
    std::vector<uint8_t> rx;   // bytes clocked in (what the fake radio returned)

    uint8_t cmd() const { return tx.empty() ? 0 : tx[0]; }
    bool    isWrite() const { return (cmd() & 0x80) != 0; }
    uint8_t reg() const { return (uint8_t) (cmd() & 0x7F); }
    size_t  payloadLen() const { return tx.empty() ? 0 : tx.size() - 1; }
};

// The fake radio behind the bus: a flat register file, plus the FIFO.
struct Bus {
    std::vector<Txn> txns;
    uint8_t          regs[128]{};
    std::vector<uint8_t> fifo;        // everything ever written to REG_FIFO
    bool             bus_initialised{false};
    int              devices_added{0};

    // Set by a test to make a register read return something specific.
    void setReg(uint8_t r, uint8_t v) { regs[r & 0x7F] = v; }
    uint8_t getReg(uint8_t r) const { return regs[r & 0x7F]; }

    void reset() {
        txns.clear();
        fifo.clear();
        for (auto& b : regs) b = 0;
        bus_initialised = false;
        devices_added = 0;
    }

    // Transactions that touched register `r`.
    size_t count(uint8_t r, bool writes) const {
        size_t n = 0;
        for (const auto& t : txns)
            if (t.reg() == r && t.isWrite() == writes) n++;
        return n;
    }
};

Bus& bus();

}  // namespace spisim
