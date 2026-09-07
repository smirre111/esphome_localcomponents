// esphome/core/hal.h shim: the busy-wait and clock helpers production calls.
//
// delayMicroseconds ADVANCES THE HARNESS CLOCK rather than doing nothing.
// That is not a convenience: production uses it to wait out a hardware
// timeout, and a no-op turns "poll until the flag sets or half a second
// passes" into an infinite loop under test. Time passing is the behaviour
// being modelled, so the shim models it.
//
// delay() stays a no-op. The B5 refactor tracked in implementation-plan.md is
// about removing those calls from the TX path, and a no-op keeps the tests
// honest about what the code would do with them gone.
#pragma once
#include <stdint.h>
#include "esphome/core/helpers.h"
#include "esp_timer.h"

namespace esphome {
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t us) { proto_sim_timer_advance_us((int64_t) us); }
inline uint32_t micros() { return (uint32_t) esp_timer_get_time(); }
} // namespace esphome
