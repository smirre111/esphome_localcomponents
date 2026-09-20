#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_timer.h"

// ---------------------------------------------------------------------------
// A recording stand-in for the node's SX1278 driver, so the REAL
// LoraInterface.cpp can be compiled and linked by the host suite.
//
// WHY THIS EXISTS. §11b's last structural entry: LoraInterface.cpp was not
// compiled by the harness at all, because the CMake shimmed LoraInterface.h
// wholesale. Two defects this year were invisible for exactly that reason —
// the uplink-aim call site went in untested, and a log line sitting inside the
// aimed critical path (3.5 ms of UART, a quarter of the guard band) was caught
// by reading the diff rather than by any test. Every fix in that file was made
// blind.
//
// The hub has had this since real_lora_tracker landed (shims/
// lora_hal_recorder.h); this is the node's half, against the node's own
// driver header.
//
// IT DOES NOT MODEL THE RADIO. It records the CALL SEQUENCE, the bytes handed
// to the FIFO, and the instants transmits fired — which is what a transmit
// sequence is made of — and it lets a test inject the interrupt flags the
// driver would have reported. Timing behaviour belongs in sim/air_channel.h,
// which does model it.
// ---------------------------------------------------------------------------

namespace loranode {

struct Recorder {
    std::vector<std::string>          calls;
    std::vector<std::vector<uint8_t>> packets;    // one per completed transmit
    std::vector<int64_t>              tx_us;      // when each fired
    std::vector<uint8_t>              staging;    // bytes since beginPacket

    // What the next lora_readInterrupts() reports. A test sets this to stand in
    // for the radio raising RX_DONE, RX_TIMEOUT or CAD_DONE; it is CONSUMED on
    // read, because the production code clears flags after acting on them and a
    // sticky flag would make one event look like an endless stream.
    uint8_t  next_interrupts{0};
    // What lora_parsePacket() hands back, and the bytes lora_read() yields.
    std::vector<uint8_t> rx_payload;
    size_t               rx_cursor{0};
    int64_t              last_tx_done_us{0};
    // Symbol timeout as last written — the window width the node asked for,
    // which is the one radio setting Mode B's geometry actually depends on.
    uint16_t sym_timeout{0};
    // DIO mapping, per pin, as last written. A window armed with DIO0 mapped to
    // CADDONE instead of RXDONE is a window that cannot hear a frame.
    uint8_t  dio_mode[6]{};

    // THE PHY, as actually programmed. Every constant in LoraTiming.h is
    // derived from these four numbers — T_sym from SF and BW, the preamble-to-T0
    // offset from the preamble length, the symbol count from SF, BW, CR and the
    // CRC flag — and until this target existed, nothing in either repository
    // checked that the radio is set to the PHY the arithmetic assumes. A
    // spreading factor off by one moves every window in the design.
    int      sf{0};
    long     bw{0};
    int      cr_denom{0};
    long     preamble_len{0};
    int      sync_word{0};
    bool     crc_on{false};

    // Called FROM lora_cad(), which is where the real DIO0 CAD-DONE interrupt
    // posts its answer into LoraInterface::lora_cad_queue_. A test answers
    // this CAD by pushing into that queue from here, and it has to be from
    // here: production resets the queue immediately before calling lora_cad,
    // so an answer seeded any earlier is wiped. That is the invariant the
    // reset exists for, not an inconvenience to work around — a stale "channel
    // free" consumed as this CAD's answer is what makes the node transmit into
    // a running burst.
    std::function<void()> on_cad;

    void reset() { *this = Recorder{}; }

    void note(const char *fn) {
        calls.emplace_back(fn);
        // A frame is closed by lora_tx, which IS the fire instant — the same
        // convention the hub's recorder uses, and for the same reason: it is
        // the only honest place to record when a frame went out.
        if (std::string(fn) == "lora_tx") {
            packets.push_back(staging);
            tx_us.push_back(esp_timer_get_time());
            last_tx_done_us = esp_timer_get_time();
            staging.clear();
        }
    }

    size_t count(const char *fn) const {
        size_t n = 0;
        for (const auto &c : calls) if (c == fn) ++n;
        return n;
    }
    // Index of the first call to `fn` at or after `from`, or calls.size().
    size_t indexOf(const char *fn, size_t from = 0) const {
        for (size_t i = from; i < calls.size(); ++i) if (calls[i] == fn) return i;
        return calls.size();
    }
};

Recorder &rec();

}  // namespace loranode
