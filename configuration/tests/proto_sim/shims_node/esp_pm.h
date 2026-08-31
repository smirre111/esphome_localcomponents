#pragma once

// Host shim for ESP-IDF power management.
//
// The node holds an ESP_PM_NO_LIGHT_SLEEP lock for the duration of a drift
// test, because light sleep stops the APB clock that esp_timer counts and the
// arrival timestamps then become part measurement, part reconstruction.
//
// Nothing sleeps on the host, so these are no-ops that record the lock state.
// The state is exposed so a test can assert the lock is RELEASED when a test
// ends — leaving it held would silently keep the node out of light sleep long
// after the bench work finished, which is a battery bug that would otherwise
// only show up in the field.

#include "esp_err.h"

#include <cstdint>

typedef enum {
    ESP_PM_CPU_FREQ_MAX,
    ESP_PM_APB_FREQ_MAX,
    ESP_PM_NO_LIGHT_SLEEP,
} esp_pm_lock_type_t;

struct proto_sim_pm_lock {
    int  acquired{0};   // net acquire/release count
    bool ever_acquired{false};
};

typedef struct proto_sim_pm_lock *esp_pm_lock_handle_t;

inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t, int, const char *,
                                    esp_pm_lock_handle_t *out) {
    if (out == nullptr) return ESP_FAIL;
    *out = new proto_sim_pm_lock();
    return ESP_OK;
}

inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) {
    if (h == nullptr) return ESP_FAIL;
    h->acquired++;
    h->ever_acquired = true;
    return ESP_OK;
}

inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) {
    if (h == nullptr) return ESP_FAIL;
    h->acquired--;
    return ESP_OK;
}

inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t h) {
    delete h;
    return ESP_OK;
}

// Power-management config. The drift test DISABLES light sleep outright rather
// than holding a NO_LIGHT_SLEEP lock, because the lock leaves automatic light
// sleep armed underneath and the node then slept through the frames it was
// supposed to be timing.
//
// Recorded rather than ignored, so a test can assert that the normal profile
// is RESTORED when the test ends. Leaving a battery node pinned at 240 MHz
// with sleep disabled would be a serious regression, and one that only shows
// up as a flat pack weeks later.
typedef struct {
    int  max_freq_mhz;
    int  min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

inline esp_pm_config_t &proto_sim_pm_state() {
    static esp_pm_config_t s{240, 40, true};
    return s;
}

inline esp_err_t esp_pm_configure(const void *cfg) {
    if (cfg == nullptr) return ESP_FAIL;
    proto_sim_pm_state() = *static_cast<const esp_pm_config_t *>(cfg);
    return ESP_OK;
}
