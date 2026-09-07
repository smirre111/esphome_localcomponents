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
        if (std::string(fn) == "lora_endPacket") {
            packets.push_back(staging);
            staging.clear();
        } else if (std::string(fn) == "lora_beginPacket") {
            staging.clear();
        }
    }
    void wrote(const uint8_t* p, size_t n) { staging.insert(staging.end(), p, p + n); }

    size_t count(const char* fn) const {
        size_t n = 0;
        for (const auto& c : calls) if (c == fn) ++n;
        return n;
    }
    void reset() { calls.clear(); packets.clear(); staging.clear(); }
};

Recorder& rec();

}  // namespace lorahal
