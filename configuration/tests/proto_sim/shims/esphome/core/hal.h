// esphome/core/hal.h shim: the busy-wait and clock helpers production calls.
//
// delay() is a no-op here. That is not a simplification to paper over: the
// B5 refactor tracked in implementation-plan.md is precisely about removing
// these calls from the TX path, and a no-op keeps the tests honest about what
// the code would do with them gone.
#pragma once
#include <stdint.h>
#include "esphome/core/helpers.h"

namespace esphome {
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline uint32_t micros() { return 0; }
} // namespace esphome
