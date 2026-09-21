#pragma once
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK             0
#define ESP_FAIL          -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
// The calibration schemes return this when the chip carries no eFuse data,
// which is the fallback path production is written to survive.
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERROR_CHECK(x) ((void)(x))

#ifdef __cplusplus
extern "C" {
#endif
// Only ever used in log strings on the host; a stable placeholder is enough.
const char* esp_err_to_name(esp_err_t code);
#ifdef __cplusplus
}
#endif
