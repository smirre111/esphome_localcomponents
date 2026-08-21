// Host-side shim for esp_bt_defs.h — only esp_bd_addr_t is used by
// lora_client.h.
#pragma once
#include <cstdint>
typedef uint8_t esp_bd_addr_t[6];
