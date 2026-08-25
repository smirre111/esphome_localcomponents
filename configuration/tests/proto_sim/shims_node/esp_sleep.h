#pragma once
// Deep-sleep stubs. CmdDispatcher::enterDeepsleep calls sysCtrl->enterDeepsleep,
// and our SystemCtrl stub is a no-op, so no real sleep API is needed — but the
// P2 wake-reason classifier reads the wakeup cause bitmask, so that is modelled
// here with a test-settable value.
#include <stdint.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors the ESP-IDF enum ordering closely enough for the bitmask API; only
// the causes the node actually enables are meaningful here.
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL       = 1,
    ESP_SLEEP_WAKEUP_EXT0      = 2,
    ESP_SLEEP_WAKEUP_EXT1      = 3,
    ESP_SLEEP_WAKEUP_TIMER     = 4,
    ESP_SLEEP_WAKEUP_TOUCHPAD  = 5,
    ESP_SLEEP_WAKEUP_ULP       = 6,
    ESP_SLEEP_WAKEUP_GPIO      = 7,
    ESP_SLEEP_WAKEUP_UART      = 8,
} esp_sleep_source_t;

uint32_t esp_sleep_get_wakeup_causes(void);

// Which EXT1 pins actually caused the wake. The node's EXT1 mask contains the
// three buttons AND the LoRa DIO lines, so the classifier must distinguish
// them — see classifyWakeReason().
uint64_t esp_sleep_get_ext1_wakeup_status(void);

// ---- harness control (not part of the ESP-IDF API) ----
// Set the bitmask a subsequent esp_sleep_get_wakeup_causes() will report, e.g.
// proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_TIMER)). Zero (the default)
// models a fresh boot rather than a deep-sleep wake.
void proto_sim_set_wakeup_causes(uint32_t causes);
// Set the EXT1 pin mask a subsequent esp_sleep_get_ext1_wakeup_status() reports.
void proto_sim_set_ext1_status(uint64_t pins);

#ifdef __cplusplus
}
#endif
