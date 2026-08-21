// Host shim for ESP-IDF esp_attr.h.
//
// On device RTC_DATA_ATTR places a variable in RTC slow memory so it survives
// deep sleep. On the host there is no deep sleep, so plain static storage has
// exactly the semantics the tests need: state persists for the life of the
// process, and a test that wants a "cold boot" resets it explicitly.
#pragma once

#define RTC_DATA_ATTR
#define RTC_RODATA_ATTR
#define RTC_FAST_ATTR
#define RTC_SLOW_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define NOINLINE_ATTR
