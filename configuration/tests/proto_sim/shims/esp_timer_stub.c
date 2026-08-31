// esp_timer stubs for the HUB target only.
//
// Split out of shims_node/esp_idf_stubs.c rather than linking that whole file:
// it also defines esp_random(), which the hub target already gets from
// shims/esp_random.c, and the duplicate is a link error.
//
// The hub needs these because the drift-test grid is paced by esp_timer
// (microsecond, APB-derived) instead of the FreeRTOS tick — the tick
// quantisation was the dominant measurement error.

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_timer.h"

// esp_timer one-shots. Nothing fires on its own; proto_sim_timer_fire_all()
// is how a test advances time.
// ---------------------------------------------------------------------------
#define TIMER_MAX 8

struct esp_timer {
    void (*callback)(void *arg);
    void *arg;
    int   armed;
    int   periodic;   /* stays armed after firing */
    int   used;
};

static struct esp_timer g_timers[TIMER_MAX];

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    if (!args || !out) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].used) {
            g_timers[i].used     = 1;
            g_timers[i].armed    = 0;
            g_timers[i].callback = args->callback;
            g_timers[i].arg      = args->arg;
            *out = &g_timers[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    (void) timeout_us;
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed    = 1;
    timer->periodic = 0;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    (void) period_us;
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed    = 1;
    timer->periodic = 1;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed = 0;
    return ESP_OK;
}
