#pragma once
#include "esp_err.h"
inline esp_err_t esp_task_wdt_add(void*)   { return ESP_OK; }
inline esp_err_t esp_task_wdt_reset()      { return ESP_OK; }

// Remove the calling task from the watchdog. frtosTasks.cpp's battery task
// does this before it exits, which is the difference between a task ending and
// a task the watchdog still expects to hear from.
static inline esp_err_t esp_task_wdt_delete(void *task) { (void) task; return ESP_OK; }
