// SPI master shim. The hub's lora.h includes this for the spi_device_handle_t
// it stores; no test drives an SPI transaction, so the handle is opaque and
// the transfer functions are absent by design — a call to one would not link,
// which is the intended signal that something reached the bus.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct spi_device_t *spi_device_handle_t;
typedef int spi_host_device_t;
