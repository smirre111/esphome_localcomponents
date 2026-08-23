// Implementation file collecting the trivial ESP-IDF stubs.
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_factory_mac[6] = { 0xE0, 0x8C, 0xFE, 0x5F, 0x9E, 0xC4 };

void proto_sim_set_factory_mac(const uint8_t mac[6]) {
    memcpy(g_factory_mac, mac, 6);
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
    (void)type;
    memcpy(mac, g_factory_mac, 6);
    return ESP_OK;
}

uint32_t esp_random(void) {
    return (uint32_t)rand() | ((uint32_t)rand() << 16);
}

int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void esp_restart(void) {
    // No-op in the host harness; tests inspect post-conditions instead.
}

// ---------------------------------------------------------------------------
// NVS: in-memory blob store.  Persists across simulated reboots (the store is
// process-global) and is cleared only by proto_sim_nvs_reset(), which models a
// factory erase.  Enough for the F-5 persistent-state paths in CmdDispatcher.
// ---------------------------------------------------------------------------
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_MAX_ENTRIES 16
#define NVS_MAX_KEY     32
#define NVS_MAX_BLOB    256

typedef struct {
    char    ns[NVS_MAX_KEY];
    char    key[NVS_MAX_KEY];
    uint8_t data[NVS_MAX_BLOB];
    size_t  len;
    int     used;
} nvs_entry_t;

static nvs_entry_t g_nvs[NVS_MAX_ENTRIES];
// Handle 0 is reserved as "invalid", so handles are index+1.  The namespace is
// recorded per handle so two namespaces cannot collide on the same key.
static char g_nvs_ns[NVS_MAX_ENTRIES][NVS_MAX_KEY];
static int  g_nvs_handles = 0;

void proto_sim_nvs_reset(void) {
    memset(g_nvs, 0, sizeof(g_nvs));
    g_nvs_handles = 0;
}

int proto_sim_nvs_entry_count(void) {
    int n = 0;
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (g_nvs[i].used) n++;
    return n;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { proto_sim_nvs_reset(); return ESP_OK; }

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t mode, nvs_handle_t* out_handle) {
    (void)mode;
    if (!namespace_name || !out_handle) return ESP_ERR_INVALID_ARG;
    if (g_nvs_handles >= NVS_MAX_ENTRIES) return ESP_FAIL;
    // NVS_READONLY on a namespace that has never been written fails on real
    // hardware; loadPersistentState() relies on that to detect "no state yet".
    if (mode == NVS_READONLY) {
        int found = 0;
        for (int i = 0; i < NVS_MAX_ENTRIES; i++)
            if (g_nvs[i].used && strcmp(g_nvs[i].ns, namespace_name) == 0) { found = 1; break; }
        if (!found) return ESP_ERR_NVS_NOT_FOUND;
    }
    int h = g_nvs_handles++;
    strncpy(g_nvs_ns[h], namespace_name, NVS_MAX_KEY - 1);
    g_nvs_ns[h][NVS_MAX_KEY - 1] = '\0';
    *out_handle = (nvs_handle_t)(h + 1);
    return ESP_OK;
}

static nvs_entry_t* nvs_find(nvs_handle_t handle, const char* key) {
    if (handle == 0 || handle > (nvs_handle_t)g_nvs_handles) return NULL;
    const char* ns = g_nvs_ns[handle - 1];
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (g_nvs[i].used && strcmp(g_nvs[i].ns, ns) == 0 && strcmp(g_nvs[i].key, key) == 0)
            return &g_nvs[i];
    return NULL;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length) {
    nvs_entry_t* e = nvs_find(handle, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!length) return ESP_ERR_INVALID_ARG;
    if (!out_value) { *length = e->len; return ESP_OK; }   // size-query form
    if (*length < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out_value, e->data, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length) {
    if (handle == 0 || handle > (nvs_handle_t)g_nvs_handles) return ESP_ERR_INVALID_ARG;
    if (length > NVS_MAX_BLOB) return ESP_ERR_NVS_INVALID_LENGTH;
    nvs_entry_t* e = nvs_find(handle, key);
    if (!e) {
        for (int i = 0; i < NVS_MAX_ENTRIES; i++)
            if (!g_nvs[i].used) { e = &g_nvs[i]; break; }
        if (!e) return ESP_FAIL;
        e->used = 1;
        strncpy(e->ns,  g_nvs_ns[handle - 1], NVS_MAX_KEY - 1);
        strncpy(e->key, key,                  NVS_MAX_KEY - 1);
    }
    memcpy(e->data, value, length);
    e->len = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void      nvs_close(nvs_handle_t handle)  { (void)handle; }

const char* esp_err_to_name(esp_err_t code) {
    static char buf[32];
    switch (code) {
    case ESP_OK:                    return "ESP_OK";
    case ESP_FAIL:                  return "ESP_FAIL";
    case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NVS_NOT_FOUND:     return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH:return "ESP_ERR_NVS_INVALID_LENGTH";
    default:
        snprintf(buf, sizeof(buf), "ESP_ERR_0x%x", (unsigned)code);
        return buf;
    }
}

// ---------------------------------------------------------------------------
// Wake cause / reset reason. Both default to "fresh power-on boot", which is
// what a test gets unless it says otherwise.
// ---------------------------------------------------------------------------
#include "esp_sleep.h"
#include "esp_timer.h"
#include <stdbool.h>

static uint32_t           g_wakeup_causes = 0;
static esp_reset_reason_t g_reset_reason  = ESP_RST_POWERON;

uint32_t esp_sleep_get_wakeup_causes(void) { return g_wakeup_causes; }
void proto_sim_set_wakeup_causes(uint32_t causes) { g_wakeup_causes = causes; }

esp_reset_reason_t esp_reset_reason(void) { return g_reset_reason; }
void proto_sim_set_reset_reason(esp_reset_reason_t reason) { g_reset_reason = reason; }

// ---------------------------------------------------------------------------
// esp_timer one-shots. Nothing fires on its own; proto_sim_timer_fire_all()
// is how a test advances time.
// ---------------------------------------------------------------------------
#define TIMER_MAX 8

struct esp_timer {
    void (*callback)(void *arg);
    void *arg;
    int   armed;
    int   periodic;   /* stays armed after firing */
    int   used;
};

static struct esp_timer g_timers[TIMER_MAX];

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    if (!args || !out) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].used) {
            g_timers[i].used     = 1;
            g_timers[i].armed    = 0;
            g_timers[i].callback = args->callback;
            g_timers[i].arg      = args->arg;
            *out = &g_timers[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    (void) timeout_us;
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed    = 1;
    timer->periodic = 0;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    (void) period_us;
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed    = 1;
    timer->periodic = 1;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->armed = 0;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (!timer) return ESP_ERR_INVALID_ARG;
    timer->used     = 0;
    timer->armed    = 0;
    timer->periodic = 0;
    return ESP_OK;
}

void proto_sim_timer_fire_all(void) {
    for (int i = 0; i < TIMER_MAX; i++) {
        if (g_timers[i].used && g_timers[i].armed) {
            /* One-shots disarm before firing; periodic timers stay armed, so a
               callback that stops its own timer is still observable. */
            if (!g_timers[i].periodic) g_timers[i].armed = 0;
            if (g_timers[i].callback) g_timers[i].callback(g_timers[i].arg);
        }
    }
}

int proto_sim_timer_armed_count(void) {
    int n = 0;
    for (int i = 0; i < TIMER_MAX; i++)
        if (g_timers[i].used && g_timers[i].armed) n++;
    return n;
}

void proto_sim_timer_reset(void) {
    memset(g_timers, 0, sizeof(g_timers));
}

// ---- esp_app_desc ----
// The node reads the running image's version string to fill the beacon's
// fwversion field. There is no image here, so hand back a fixed one.
#include "esp_app_desc.h"
static const esp_app_desc_t s_app_desc = { "9.8.7", "proto_sim" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_app_desc; }
