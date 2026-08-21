// ESP-IDF logging shim — same printf-to-stderr pattern as the hub shim,
// with a level cap controlled by PROTO_SIM_NODE_LOG_LEVEL.
#pragma once
#include <stdio.h>

#ifndef PROTO_SIM_NODE_LOG_LEVEL
#define PROTO_SIM_NODE_LOG_LEVEL 3
#endif

#define ESP_LOG_AT_(level, tag, fmt, ...) \
    do { if (PROTO_SIM_NODE_LOG_LEVEL >= (level)) \
        fprintf(stderr, "[%s] " fmt "\n", (tag), ##__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, fmt, ...)       ESP_LOG_AT_(1, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)       ESP_LOG_AT_(2, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)       ESP_LOG_AT_(3, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)       ESP_LOG_AT_(4, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...)       ESP_LOG_AT_(5, tag, fmt, ##__VA_ARGS__)
#define ESP_LOG_BUFFER_HEX(tag, buf, len) (void)(tag); (void)(buf); (void)(len)
