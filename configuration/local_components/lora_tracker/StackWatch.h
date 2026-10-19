#pragma once
// StackWatch.h -- when to log the stack high-water marks of the tasks the hub's
// LoRa components run on (tracker send task, the loop task, the esp_timer task
// that builds ModeTest / MAC ping / drift frames).
//
// Added 2026-10-19 (S8 bench): with MAC_WIRE_V1 the send task holds a 255-byte
// v1 frame on its 4 KB stack and the esp_timer task (3.5 KB) frames and seals
// a v1 frame per mark. A low-rate readout lets a bench session see the margin
// instead of discovering it as a stack overflow.
//
// Pure policy (host-tested): first report kFirstAfterUs after boot, then every
// kEveryUs. HUB-ONLY.

#include <cstdint>

namespace stackwatch
{

static constexpr int64_t kFirstAfterUs = 60LL * 1000000;    // boot + 60 s
static constexpr int64_t kEveryUs      = 300LL * 1000000;   // then every 5 min

// True when a report is due at now_us; *last_us is the previous report (0 =
// none yet) and is advanced when it returns true.
inline bool due(int64_t now_us, int64_t *last_us)
{
    const int64_t next = (*last_us == 0) ? kFirstAfterUs : *last_us + kEveryUs;
    if (now_us < next)
        return false;
    *last_us = now_us;
    return true;
}

}  // namespace stackwatch
