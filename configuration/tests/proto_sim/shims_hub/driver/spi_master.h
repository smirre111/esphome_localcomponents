// ESP-IDF SPI master shim: enough of the API for the SX1278 driver, backed by
// spisim::bus(). Only the three entry points lora.cpp uses are declared, so a
// driver that reaches for a fourth fails to link rather than silently doing
// nothing.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct spi_device_t *spi_device_handle_t;
typedef int spi_host_device_t;

#define SPI1_HOST 0
#define SPI2_HOST 1
#define SPI3_HOST 2
#define HSPI_HOST SPI2_HOST
#define VSPI_HOST SPI3_HOST

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;

typedef struct {
    uint8_t command_bits;
    uint8_t address_bits;
    uint8_t dummy_bits;
    uint8_t mode;
    uint16_t duty_cycle_pos;
    uint16_t cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    int clock_speed_hz;
    int input_delay_ns;
    int spics_io_num;
    uint32_t flags;
    int queue_size;
    void (*pre_cb)(void*);
    void (*post_cb)(void*);
} spi_device_interface_config_t;

typedef struct {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t length;      // in BITS
    size_t rxlength;    // in bits
    void *user;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg,
                             int dma_chan);
esp_err_t spi_bus_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev_cfg,
                             spi_device_handle_t *handle);
esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *trans);

#ifdef __cplusplus
}
#endif
