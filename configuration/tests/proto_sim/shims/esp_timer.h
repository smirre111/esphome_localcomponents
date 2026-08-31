#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

// ---------------------------------------------------------------------------
// One-shot timer API. Real esp_timer runs callbacks from its own task; here
// nothing fires on its own — a test decides when time passes by calling
// proto_sim_timer_fire_all(). That keeps the P2b resume-fallback test
// deterministic instead of racing a wall clock.
// ---------------------------------------------------------------------------

typedef struct esp_timer *esp_timer_handle_t;

typedef enum {
    ESP_TIMER_TASK = 0,
    ESP_TIMER_ISR  = 1,
} esp_timer_dispatch_t;

typedef struct {
    void (*callback)(void *arg);
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
// Periodic timers stay armed after firing, unlike one-shots — proto_sim_timer_fire_all()
// re-arms them so a test can observe repeated behaviour (e.g. REGISTER retries).
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

// ---- harness control (not part of the ESP-IDF API) ----
// Fire every armed one-shot and disarm it, as if its timeout had elapsed.
void proto_sim_timer_fire_all(void);
// How many one-shots are currently armed — lets a test assert that a fallback
// was cancelled rather than merely not fired.
int  proto_sim_timer_armed_count(void);
// Drop all registered timers (call between tests).
void proto_sim_timer_reset(void);

#ifdef __cplusplus
}
#endif
