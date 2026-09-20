#pragma once
#include "driver/gpio.h"

// RTC-domain GPIO. LoraInterface.cpp pulls DIO0/DIO1 into the RTC domain so a
// light-sleep wake can be armed on them; nothing here is modelled.
typedef enum {
    RTC_GPIO_MODE_INPUT_ONLY = 0,
    RTC_GPIO_MODE_OUTPUT_ONLY,
    RTC_GPIO_MODE_INPUT_OUTPUT,
    RTC_GPIO_MODE_DISABLED,
} rtc_gpio_mode_t;

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t rtc_gpio_init(gpio_num_t pin);
esp_err_t rtc_gpio_deinit(gpio_num_t pin);
esp_err_t rtc_gpio_set_direction(gpio_num_t pin, rtc_gpio_mode_t mode);
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_en(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
int       rtc_gpio_is_valid_gpio(gpio_num_t pin);
#ifdef __cplusplus
}
#endif
