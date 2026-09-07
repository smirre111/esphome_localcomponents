// The fake SX1278 behind the SPI bus.
//
// It answers reads out of a flat register file and appends FIFO writes to a
// vector. That is enough for what these tests ask: not "does the radio
// transmit", but "how many transactions does a frame cost, and do the bytes
// arrive in order".

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "spi_recorder.h"

#include <cstring>

namespace spisim {
Bus& bus() { static Bus b; return b; }
}  // namespace spisim

extern "C" {

esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int) {
    spisim::bus().bus_initialised = true;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t,
                             const spi_device_interface_config_t*,
                             spi_device_handle_t* handle) {
    spisim::bus().devices_added++;
    // A non-null opaque handle; the driver only passes it back to us.
    if (handle) *handle = reinterpret_cast<spi_device_handle_t>(1);
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t, spi_transaction_t* t) {
    if (!t) return ESP_ERR_INVALID_ARG;

    auto& b = spisim::bus();
    const size_t nbytes = t->length / 8;

    spisim::Txn rec;
    rec.tx.assign(nbytes, 0);
    if (t->tx_buffer)
        std::memcpy(rec.tx.data(), t->tx_buffer, nbytes);

    rec.rx.assign(nbytes, 0);
    if (nbytes >= 1) {
        const uint8_t cmd  = rec.tx[0];
        const uint8_t reg  = (uint8_t) (cmd & 0x7F);
        const bool    write = (cmd & 0x80) != 0;

        if (write) {
            // The SX1278 FIFO auto-increments; every other register is one
            // byte and only the first payload byte reaches it.
            if (reg == 0x00) {
                for (size_t i = 1; i < nbytes; ++i) b.fifo.push_back(rec.tx[i]);
            } else if (nbytes >= 2) {
                b.regs[reg] = rec.tx[1];
            }
        } else {
            for (size_t i = 1; i < nbytes; ++i) rec.rx[i] = b.regs[reg];
        }
    }

    if (t->rx_buffer)
        std::memcpy(t->rx_buffer, rec.rx.data(), nbytes);

    b.txns.push_back(std::move(rec));
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t, uint32_t)        { return ESP_OK; }
void      gpio_pad_select_gpio(uint32_t)              {}

}  // extern "C"

extern "C" void esp_rom_gpio_pad_select_gpio(uint32_t) {}
