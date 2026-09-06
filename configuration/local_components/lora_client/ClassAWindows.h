#pragma once

#include <stdint.h>

#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// ClassAWindows — Mode C receive windows, referenced to the node's OWN
// transmission.
//
//   T0_uplink = t_txdone - n_sym(len) * T_sym
//   RX1 opens at T0_uplink + D1 - (T_pre + G)
//   RX2 opens at T0_uplink + D2 - (T_pre + G)
//
// This needs NO clock agreement: the reference is a transmission the node timed
// itself, so there is no drift, no ppm, no beacon and no grid. That is exactly
// why it works for a node that has just booted with no phase — which is every
// automatic-mode wake.
//
// The anchor already exists in the firmware. lora_endPacket(async) maps DIO0 to
// TxDone, the handler task already services it, and the DIO0 ISR already takes
// esp_timer_get_time() as its first statement for EVERY edge, TxDone included.
// RX1/RX2 are that existing timestamp plus two esp_timer one-shots.
//
// WHAT THIS DOES NOT HAVE IS A HUB. See the helpers at the bottom and
// implementation-plan.md section 5.4: the hub replies at +750 ms as a 17-copy
// burst, and both windows fall between copies. Class A needs a single copy at a
// precisely known offset; a burst is the opposite construction.
//
// Dependency-free. See implementation-plan.md section 5, test-plan.md 6.1.
// ---------------------------------------------------------------------------

namespace classa
{

using namespace loratiming;

// Protocol constants, chosen (not measured) and shared by both ends.
static constexpr uint32_t kRx1DelayUs = 1000000;
static constexpr uint32_t kRx2DelayUs = 2000000;

// Same arming construction as Mode B: T_pre + G before the expected T0.
static constexpr uint32_t kArmLeadUs = timedgrid::kArmLeadUs;
static constexpr uint32_t kWindowUs  = timedgrid::kWindowUs;

// The node's own SFD end, from its own TxDone. Payload-DEPENDENT: using
// t_txdone directly instead is wrong by n_sym(len)*T_sym, which ranges from
// 18.4 ms to 92.2 ms. That error makes short frames work and long frames fail,
// which is the worst possible failure signature — it looks like interference.
constexpr int64_t t0UplinkUs(int64_t t_txdone_us, uint32_t uplink_len)
{
    return t0FromRxDoneUs(t_txdone_us, uplink_len);
}

constexpr int64_t rx1OpenUs(int64_t t0_uplink_us)
{
    return t0_uplink_us + (int64_t) kRx1DelayUs - (int64_t) kArmLeadUs;
}
constexpr int64_t rx2OpenUs(int64_t t0_uplink_us)
{
    return t0_uplink_us + (int64_t) kRx2DelayUs - (int64_t) kArmLeadUs;
}
constexpr int64_t rx1CloseUs(int64_t t0) { return rx1OpenUs(t0) + kWindowUs; }
constexpr int64_t rx2CloseUs(int64_t t0) { return rx2OpenUs(t0) + kWindowUs; }

// What the node does after each window.
enum class WakeAction : uint8_t { OpenRx1, OpenRx2, Sleep };

struct WakeState
{
    bool rx1_done     = false;
    bool rx1_had_data = false;
    bool rx2_done     = false;
    // Tier 1: the hub says its per-node queue is empty. Proto3 zero must mean
    // "keep waiting", i.e. today's behaviour, so a node that has not been told
    // anything does not sleep early.
    bool sleep_ok     = false;
};

constexpr WakeAction nextAction(const WakeState &s)
{
    if (s.sleep_ok)                    return WakeAction::Sleep;
    if (!s.rx1_done)                   return WakeAction::OpenRx1;
    if (s.rx1_had_data)                return WakeAction::Sleep;
    if (!s.rx2_done)                   return WakeAction::OpenRx2;
    return WakeAction::Sleep;
}

// --- What the hub does today ----------------------------------------------
//
// send_timesync() is scheduled with set_timeout(..., 750, ...) and goes through
// the ordinary burst path, so copies land at 750 ms + 88 ms * i.
static constexpr uint32_t kTodaysHubReplyDelayUs = 750000;

constexpr int64_t todaysHubCopyUs(uint8_t i)
{
    return (int64_t) kTodaysHubReplyDelayUs + (int64_t) i * kBurstCopyStrideUs;
}

// Does copy i's T0 land inside [open, close)?
constexpr bool copyLandsIn(int64_t open_us, int64_t close_us, uint8_t i)
{
    const int64_t t0 = todaysHubCopyUs(i);
    return t0 >= open_us && t0 < close_us;
}

}  // namespace classa
