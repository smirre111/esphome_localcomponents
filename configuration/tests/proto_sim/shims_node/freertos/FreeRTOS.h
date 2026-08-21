// Host-side FreeRTOS API shim — synchronous fakes for queues / semaphores
// suitable for unit-testing the protocol layer of BlindsESP. Tasks are NOT
// spawned; tests drive xQueueReceive consumers by hand.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>

using TickType_t       = uint32_t;
using BaseType_t       = int;
using UBaseType_t      = unsigned int;
using StackType_t      = uint8_t;
using TaskHandle_t     = void*;
using TimerHandle_t    = void*;

constexpr TickType_t portMAX_DELAY      = 0xFFFFFFFFu;
constexpr BaseType_t pdTRUE             = 1;
constexpr BaseType_t pdFALSE            = 0;
constexpr BaseType_t pdPASS             = 1;
constexpr BaseType_t pdFAIL             = 0;

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
inline TickType_t portTICK_PERIOD_MS_VAL()    { return 1; }
#define portTICK_PERIOD_MS portTICK_PERIOD_MS_VAL()

// Spinlock (no-op on host).
struct portMUX_TYPE { int unused{0}; };
#define portMUX_INITIALIZER_UNLOCKED {0}
inline void portENTER_CRITICAL(portMUX_TYPE*)      {}
inline void portEXIT_CRITICAL(portMUX_TYPE*)       {}
inline void portENTER_CRITICAL(portMUX_TYPE& mux)  { (void)mux; }
inline void portEXIT_CRITICAL(portMUX_TYPE& mux)   { (void)mux; }
inline void vTaskDelay(TickType_t)                  {}

// Queue — back-store is std::deque<std::vector<uint8_t>>. Items are
// item_size bytes; opaque to the producer/consumer.
struct QueueDef {
    size_t   item_size{0};
    size_t   capacity{0};
    std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = QueueDef*;

inline QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t item_size) {
    auto* q = new QueueDef();
    q->item_size = item_size;
    q->capacity  = capacity;
    return q;
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t /*wait*/) {
    if (!q) return pdFALSE;
    if (q->items.size() >= q->capacity) return pdFALSE;
    std::vector<uint8_t> blob(q->item_size);
    if (item) std::copy_n(static_cast<const uint8_t*>(item), q->item_size, blob.begin());
    q->items.push_back(std::move(blob));
    return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t /*wait*/) {
    if (!q || q->items.empty()) return pdFALSE;
    auto blob = std::move(q->items.front());
    q->items.pop_front();
    if (out) std::copy_n(blob.begin(), q->item_size, static_cast<uint8_t*>(out));
    return pdTRUE;
}
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    return q ? static_cast<UBaseType_t>(q->items.size()) : 0;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline void vQueueAddToRegistry(QueueHandle_t, const char*) {}

// Semaphore — degenerate (always succeeds).
using SemaphoreHandle_t = void*;
inline SemaphoreHandle_t xSemaphoreCreateMutex()          { return (SemaphoreHandle_t)(uintptr_t)1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t)             { return pdTRUE; }

// Task notification — no-op.
inline BaseType_t xTaskNotifyGive(TaskHandle_t) { return pdTRUE; }
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline TickType_t xTaskGetTickCount() { return 0; }

// Task creation — degenerate (we don't run task loops).
inline BaseType_t xTaskCreate(void(*)(void*), const char*, uint32_t,
                              void*, UBaseType_t, TaskHandle_t*) { return pdPASS; }
inline void vTaskDelete(TaskHandle_t) {}
