// Host-side shim for ESP-IDF esp_random().
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);
#ifdef __cplusplus
}
#endif
