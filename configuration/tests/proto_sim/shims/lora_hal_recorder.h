#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// A recording stand-in for the SX1278 driver, so the REAL lora_tracker.cpp can
// be compiled and linked by the host suite.
//
// Why this exists: until now no test compiled lora_tracker.cpp — it was
// replaced wholesale by a shim class. That blind spot hid two real defects
// found only by review: a msgid that wedges the link after ~1024 pings, and a
// TimedGrid.h include inside #ifdef USE_OTA that breaks any ESPHome build
// without OTA. Neither is exotic; both are invisible if the file never
// compiles.
//
// This does not model the radio. It records the CALL SEQUENCE and the bytes
// handed to the FIFO, which is what burst structure and per-frame TX policy are
// made of. Timing behaviour belongs in sim/air_channel.h, which does model it.
// ---------------------------------------------------------------------------

namespace lorahal {

struct Recorder {
    std::vector<std::string>          calls;
    std::vector<std::vector<uint8_t>> packets;   // one entry per endPacket
    std::vector<uint8_t>              staging;   // bytes since the last beginPacket

    void note(const char* fn) {
        calls.emplace_back(fn);
        // A frame is closed by lora_tx, not by lora_endPacket.
        //
        // lora_tx IS the fire instant, and since B5's prepare/fire split the
        // tracker calls it directly — lora_endPacket (which wraps fire + wait)
        // is no longer on the transmit path at all. Keying on lora_tx records
        // what actually went out, under either arrangement.
        const std::string name(fn);
        if (name == "lora_tx") {
            packets.push_back(staging);
            staging.clear();
        } else if (name == "lora_beginPacket") {
            staging.clear();
        }
    }
    void wrote(const uint8_t* p, size_t n) { staging.insert(staging.end(), p, p + n); }

    size_t count(const char* fn) const {
        size_t n = 0;
        for (const auto& c : calls) if (c == fn) ++n;
        return n;
    }
    // Packets the fake radio will hand back, oldest first. lora_receive_packet
    // pops one per call and returns 0 when the queue is empty, which is what
    // the poll loop sees on an idle channel.
    std::vector<std::vector<uint8_t>> inbox;

    // What lora_lastTxDoneUs() reports. Set by a test that cares.
    int64_t txdone_us{0};

    void queueRx(std::vector<uint8_t> frame) { inbox.push_back(std::move(frame)); }

    void reset() {
        calls.clear(); packets.clear(); staging.clear(); inbox.clear();
        txdone_us = 0;
    }
};

Recorder& rec();

}  // namespace lorahal
