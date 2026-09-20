#pragma once
#include <stdint.h>
#include "esp_err.h"

// Minimal gpio enum so common.h's static gpio_num_t constants compile.
typedef int32_t gpio_num_t;

// Enough of the GPIO API for the REAL LoraInterface.cpp to compile and link.
// It configures DIO0/DIO1 as interrupt sources and a light-sleep wake source;
// none of that is modelled, but all of it has to resolve.
typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
    GPIO_INTR_LOW_LEVEL,
    GPIO_INTR_HIGH_LEVEL,
} gpio_int_type_t;

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

#define ESP_INTR_FLAG_IRAM 0

// The ISR handler type LoraInterface.h names in its own signatures.
typedef void (*gpio_isr_t)(void *arg);

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*fn)(void *), void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int       gpio_get_level(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_wakeup_disable(gpio_num_t pin);
#ifdef __cplusplus
}
#endif
