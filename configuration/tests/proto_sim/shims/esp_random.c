#include "esp_random.h"
#include <stdlib.h>

uint32_t esp_random(void) {
    return (uint32_t)rand() | ((uint32_t)rand() << 16);
}
