#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

// Reset reasons. The P2 wake-reason classifier uses these to tell a planned
// boot from a crash-like reset, so a node stuck reset-looping reports
// WAKE_UNKNOWN rather than masquerading as a normal boot.
typedef enum {
    ESP_RST_UNKNOWN   = 0,
    ESP_RST_POWERON   = 1,
    ESP_RST_EXT       = 2,
    ESP_RST_SW        = 3,
    ESP_RST_PANIC     = 4,
    ESP_RST_INT_WDT   = 5,
    ESP_RST_TASK_WDT  = 6,
    ESP_RST_WDT       = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT  = 9,
    ESP_RST_SDIO      = 10,
    ESP_RST_USB       = 11,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

// ---- harness control (not part of the ESP-IDF API) ----
void proto_sim_set_reset_reason(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif
