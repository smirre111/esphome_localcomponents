// GPIO shim. Levels are not recorded: the driver toggles CS around every
// transaction and asserting on that would only restate the loop it sits in.
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef int32_t gpio_num_t;

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT   = 1,
    GPIO_MODE_OUTPUT  = 2,
} gpio_mode_t;

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
void      gpio_pad_select_gpio(uint32_t pin);
#ifdef __cplusplus
}
#endif

// ESP-IDF renamed gpio_pad_select_gpio to esp_rom_gpio_pad_select_gpio; the
// driver calls the newer name.
#ifdef __cplusplus
extern "C" {
#endif
void esp_rom_gpio_pad_select_gpio(uint32_t pin);
#ifdef __cplusplus
}
#endif
