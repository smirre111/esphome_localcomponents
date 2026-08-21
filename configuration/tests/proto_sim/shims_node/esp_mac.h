#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <string.h>

typedef enum {
    ESP_MAC_EFUSE_FACTORY = 0,
    ESP_MAC_WIFI_STA      = 1,
} esp_mac_type_t;

// Test-side hook so scenarios can pin the simulated factory MAC.
#ifdef __cplusplus
extern "C" {
#endif
void proto_sim_set_factory_mac(const uint8_t mac[6]);
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
#ifdef __cplusplus
}
#endif
