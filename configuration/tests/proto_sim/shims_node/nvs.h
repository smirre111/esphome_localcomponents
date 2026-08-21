// Host shim for ESP-IDF NVS, backed by an in-memory blob store.
//
// CmdDispatcher.cpp uses NVS for F-5 persistent state (tx/rx msgids + the hub
// base nonce) so an unexpected reboot can resume the encrypted session without
// a mandatory re-login.  The harness needs real read/write semantics for that:
// reboot scenarios save state, "reboot", and expect to read it back.
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_NVS_BASE            0x1100
#define ESP_ERR_NVS_NOT_FOUND       (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_LENGTH  (ESP_ERR_NVS_BASE + 0x0a)

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY  = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t mode, nvs_handle_t* out_handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
void      nvs_close(nvs_handle_t handle);

// ---- harness control (not part of the ESP-IDF API) ----
// Wipe the simulated flash — models a factory erase / fresh chip.
void proto_sim_nvs_reset(void);
// Number of committed blobs; lets tests assert write-throttling behaviour.
int  proto_sim_nvs_entry_count(void);

#ifdef __cplusplus
}
#endif
