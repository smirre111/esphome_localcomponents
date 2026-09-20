#pragma once
#include "esp_err.h"
#include "esp_log.h"

// ESP_RETURN_ON_ERROR and friends, reduced to what LoraInterface.cpp uses.
// The macros carry control flow, so they are real rather than no-ops: a stub
// that swallowed an error return would change the behaviour under test.
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                                  \
    do {                                                                       \
        esp_err_t err_rc_ = (x);                                               \
        if (err_rc_ != ESP_OK) return err_rc_;                                 \
    } while (0)

#define ESP_RETURN_ON_FALSE(a, err_code, tag, fmt, ...)                        \
    do { if (!(a)) return (err_code); } while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, fmt, ...)                          \
    do {                                                                       \
        esp_err_t err_rc_ = (x);                                               \
        if (err_rc_ != ESP_OK) { ret = err_rc_; goto goto_tag; }                \
    } while (0)

#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, tag, fmt, ...)                \
    do { if (!(a)) { ret = (err_code); goto goto_tag; } } while (0)
