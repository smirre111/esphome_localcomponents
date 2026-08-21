#pragma once
#include "esp_err.h"
inline esp_err_t esp_task_wdt_add(void*)   { return ESP_OK; }
inline esp_err_t esp_task_wdt_reset()      { return ESP_OK; }
