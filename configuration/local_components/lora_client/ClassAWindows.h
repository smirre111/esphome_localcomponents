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

// --- Arming, pass by pass ---------------------------------------------------
//
// The receive task decides what to arm once per loop pass, and a pass can come
// round while the window it just opened is still listening: the window's
// outcome (RxDone or RX timeout) is reported later, from the interrupt task. The
// sequence still says "RX1 pending", the RX1 instant is now in the past, and the
// old arm clamped that to 1 us and armed it AGAIN — which idles the radio in the
// middle of RX1 and opens a second window. Measured 2026-09-14 on node 2:
// "Class A RX1: empty (timeout)" then "Class A RX2: empty (timeout)" 30 ms later,
// where RX2 belongs 1 s later — so the real RX2 never opened.
//
// The rule: a window instant that has already been armed is never armed twice.
// While its outcome is out, the task only RECHECKS shortly after the window
// closes — without touching the radio — and arms the next window once the
// outcome is in. The recheck is floored so a long frame still being received
// past the window's nominal close cannot spin the loop at 1 us.
enum class ArmDecision : uint8_t { None, Arm, Recheck };

struct ArmPlan
{
    ArmDecision what;
    int64_t     delay_us;
};

static constexpr int64_t kRecheckMarginUs = 20000;   // after the window's nominal close
static constexpr int64_t kRecheckMinUs    = 10000;   // never sooner than this

// arm_instant_us:   the pending window's arm instant (0 = nothing pending)
// armed_instant_us: the instant the last Class A one-shot was armed for
// close_us:         the pending window's nominal close
// lead_us:          the caller's software arm lead
constexpr ArmPlan armPlan(int64_t arm_instant_us, int64_t armed_instant_us,
                          int64_t close_us, int64_t now_us, int64_t lead_us)
{
    if (arm_instant_us == 0)
        return ArmPlan{ArmDecision::None, 0};
    if (arm_instant_us == armed_instant_us)
    {
        const int64_t d = close_us + kRecheckMarginUs - now_us;
        return ArmPlan{ArmDecision::Recheck, (d < kRecheckMinUs) ? kRecheckMinUs : d};
    }
    const int64_t d = arm_instant_us - lead_us - now_us;
    return ArmPlan{ArmDecision::Arm, (d < 1) ? 1 : d};
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
