// FreeRTOSConfig.h shim. On target this carries the kernel tuning constants;
// on host only the few the tracker reads matter.
#pragma once
#include "freertos/FreeRTOS.h"

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif
#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 25
#endif
