#pragma once
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK             0
#define ESP_FAIL          -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(x) ((void)(x))

#ifdef __cplusplus
extern "C" {
#endif
// Only ever used in log strings on the host; a stable placeholder is enough.
const char* esp_err_to_name(esp_err_t code);
#ifdef __cplusplus
}
#endif
