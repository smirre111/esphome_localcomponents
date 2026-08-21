// Host-side shim for ESPHome's log.h. ESP_LOG* macros become printf-to-stderr
// so production log lines are visible during test runs but can be filtered.
#pragma once
#include <cstdio>

#ifndef PROTO_SIM_REAL_LOG_LEVEL
#define PROTO_SIM_REAL_LOG_LEVEL 3  // 0=off, 1=E, 2=W, 3=I, 4=D, 5=CONFIG
#endif

#define ESP_LOG_AT_(level, tag, fmt, ...) \
    do { if (PROTO_SIM_REAL_LOG_LEVEL >= (level)) \
        std::fprintf(stderr, "[%s] " fmt "\n", (tag), ##__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, fmt, ...)       ESP_LOG_AT_(1, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)       ESP_LOG_AT_(2, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)       ESP_LOG_AT_(3, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)       ESP_LOG_AT_(4, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGCONFIG(tag, fmt, ...)  ESP_LOG_AT_(5, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...)       ESP_LOG_AT_(6, tag, fmt, ##__VA_ARGS__)
