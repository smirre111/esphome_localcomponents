
#include "esphome/core/hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include <string.h>

#include "lora.h"


// #define CONFIG_CS_GPIO    19
// #define CONFIG_RST_GPIO   16
// #define CONFIG_MISO_GPIO  5
// #define CONFIG_MOSI_GPIO  18
// #define CONFIG_SCK_GPIO   17  

#define CONFIG_SCK_GPIO  5 // GPIO5  -- SX1278's SCK
#define CONFIG_MISO_GPIO 19 // GPIO19 -- SX1278's MISO
#define CONFIG_MOSI_GPIO 27 // GPIO27 -- SX1278's MOSI
#define CONFIG_CS_GPIO   18 // GPIO18 -- SX1278's CS
#define CONFIG_RST_GPIO  14 // GPIO14 -- SX1278's RESET
#define CONFIG_DI0  26 // GPIO26 -- SX1278's IRQ(Interrupt Request)



/*
 * Register definitions
 */
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_OCP 0x0b
#define REG_LNA 0x0c
#define REG_FIFO_ADDR_PTR 0x0d
#define REG_FIFO_TX_BASE_ADDR 0x0e
#define REG_FIFO_RX_BASE_ADDR 0x0f
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1a
#define REG_RSSI_VALUE 0x1b
#define REG_MODEM_CONFIG_1 0x1d
#define REG_MODEM_CONFIG_2 0x1e
#define REG_SYMB_TIMEOUT_LSB 0x1f
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MAX_PAYLOAD_LENGTH 0x23
#define REG_MODEM_CONFIG_3 0x26
#define REG_PPM_CORRECTION 0x27
#define REG_FREQ_ERROR_MSB 0x28
#define REG_FREQ_ERROR_MID 0x29
#define REG_FREQ_ERROR_LSB 0x2a
#define REG_RSSI_WIDEBAND 0x2c
#define REG_DETECTION_OPTIMIZE 0x31
#define REG_INVERTIQ 0x33
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD 0x39
#define REG_INVERTIQ2 0x3b
#define REG_DIO_MAPPING_1 0x40
#define REG_DIO_MAPPING_2 0x41
#define REG_VERSION 0x42
#define REG_TCXO 0x4b
#define REG_PA_DAC 0x4d

/*
 * Transceiver modes
 */
#define MODE_LONG_RANGE_MODE 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05
#define MODE_RX_SINGLE 0x06
#define MODE_CAD 0x07

/*
 * PA configuration
 */
#define PA_BOOST 0x80

#define RF_MID_BAND_THRESHOLD 525E6
#define RSSI_OFFSET_HF_PORT 157
#define RSSI_OFFSET_LF_PORT 164

#define MAX_PKT_LENGTH 255
/*
 * IRQ masks
 */
#define IRQ_TX_DONE_MASK 0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK 0x40

#define PA_OUTPUT_RFO_PIN 0
#define PA_OUTPUT_PA_BOOST_PIN 1

#define TIMEOUT_RESET 100


static const char *TAG = "Lora";


static spi_device_handle_t __spi;

static int __implicit;
static long __frequency;
static volatile int _packetIndex = 0;

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) (bitvalue ? bitSet(value, bit) : bitClear(value, bit))

SemaphoreHandle_t xSemaphore = xSemaphoreCreateMutex();

/**
 * Write a value to a register.
 * @param reg Register index.
 * @param val Value to write.
 */
void lora_write_reg(int reg, int val)
{
   // uint8_t out[1] = {static_cast<uint8_t>(val)};
   // uint8_t in[2];

   // spi_transaction_t t = {
   //     .flags = 0,
   //     .cmd = 0,
   //     .addr = uint64_t(static_cast<uint8_t>(0x80) | static_cast<uint8_t>(reg)),
   //     .length = 8,
   //     .rxlength = 8,
   //     .user = NULL,
   //     .tx_buffer = out,
   //     .rx_buffer = in};
   uint8_t reg8, val8;
   reg8 = static_cast<uint8_t>(reg);
   val8 = static_cast<uint8_t>(val);
   uint8_t cRead = 0x80;
   uint8_t cmd = static_cast<uint8_t>(cRead | reg8);
   uint8_t out[2] = {cmd, val8};
   uint8_t in[2];

   spi_transaction_t t{};
   t.flags = 0;
   t.length = 8 * sizeof(out);
   t.tx_buffer = out;
   t.rx_buffer = in;

   if (xSemaphoreTake(xSemaphore, (TickType_t)1) == pdTRUE)
   {
      /* We were able to obtain the semaphore and can now access the
            shared resource. */

      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 0);
      spi_device_transmit(__spi, &t);
      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 1);

      /* We have finished accessing the shared resource.  Release the
            semaphore. */
      xSemaphoreGive(xSemaphore);
   }
   else
   {
      /* We could not obtain the semaphore and can therefore not access
            the shared resource safely. */
   }
}

/**
 * Read the current value of a register.
 * @param reg Register index.
 * @return Value of the register.
 */
int lora_read_reg(int reg)
{
   // uint8_t out[2] = {static_cast<uint8_t>(reg), 0xff};
   // uint8_t rx_buf[2];

   // spi_transaction_t t = {
   //     .flags = 0,
   //     .cmd = 0,
   //     .addr = uint64_t(reg),
   //     .length = 8,
   //     .rxlength = 8,
   //     .user = NULL,
   //     .tx_buffer = out,
   //     .rx_buffer = (void *)rx_buf};

   uint8_t out[2] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(0xff)};
   uint8_t in[2];

   spi_transaction_t t{};
   t.flags = 0;
   t.length = 8 * sizeof(out);
   t.tx_buffer = out;
   t.rx_buffer = in;

   if (xSemaphoreTake(xSemaphore, (TickType_t)1) == pdTRUE)
   {
      /* We were able to obtain the semaphore and can now access the
            shared resource. */

      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 0);
      spi_device_transmit(__spi, &t);
      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 1);

      /* We have finished accessing the shared resource.  Release the
            semaphore. */
      xSemaphoreGive(xSemaphore);
   }
   else
   {
      /* We could not obtain the semaphore and can therefore not access
            the shared resource safely. */
      ESP_LOGE(TAG, "Could not read semaphore");
      
   }

   // return rx_buf[1];
   return int(in[1]);
}

/**
 * Write a value to a register.
 * @param reg Register index.
 * @param val Value to write.
 */
void lora_write_reg1(int reg, int val)
{
   // uint8_t out[1] = {static_cast<uint8_t>(val)};
   // uint8_t in[2];

   // spi_transaction_t t = {
   //     .flags = 0,
   //     .cmd = 0,
   //     .addr = uint64_t(static_cast<uint8_t>(0x80) | static_cast<uint8_t>(reg)),
   //     .length = 8,
   //     .rxlength = 8,
   //     .user = NULL,
   //     .tx_buffer = out,
   //     .rx_buffer = in};
   uint8_t reg8, val8;
   reg8 = static_cast<uint8_t>(reg);
   val8 = static_cast<uint8_t>(val);
   uint8_t cRead = 0x80;

   uint8_t cmd = static_cast<uint8_t>(cRead | reg8);
   uint8_t out[2] = {cmd, val8};
   uint8_t in[2];

   spi_transaction_t t{};
   t.flags = 0;
   t.length = 8 * sizeof(out);
   t.tx_buffer = out;
   t.rx_buffer = in;

   // static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
   // if (xSemaphoreTakeFromISR(xSemaphore, &xHigherPriorityTaskWoken))
   {
      /* We were able to obtain the semaphore and can now access the
            shared resource. */

      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 0);
      spi_device_transmit(__spi, &t);
      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 1);

      /* We have finished accessing the shared resource.  Release the
            semaphore. */
      // xSemaphoreGiveFromISR(xSemaphore, &xHigherPriorityTaskWoken);
   }
   // else
   {
      /* We could not obtain the semaphore and can therefore not access
            the shared resource safely. */
   }
}

/**
 * Read the current value of a register.
 * @param reg Register index.
 * @return Value of the register.
 */
int lora_read_reg1(int reg)
{
   // uint8_t out[2] = {static_cast<uint8_t>(reg), 0xff};
   // uint8_t rx_buf[2];

   // spi_transaction_t t = {
   //     .flags = 0,
   //     .cmd = 0,
   //     .addr = uint64_t(reg),
   //     .length = 8,
   //     .rxlength = 8,
   //     .user = NULL,
   //     .tx_buffer = out,
   //     .rx_buffer = (void *)rx_buf};

   uint8_t out[2] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(0xff)};
   uint8_t in[2];

   spi_transaction_t t{};
   t.flags = 0;
   t.length = 8 * sizeof(out);
   t.tx_buffer = out;
   t.rx_buffer = in;

   // static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
   // if (xSemaphoreTakeFromISR(xSemaphore, &xHigherPriorityTaskWoken))
   {
      /* We were able to obtain the semaphore and can now access the
            shared resource. */

      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 0);
      spi_device_transmit(__spi, &t);
      gpio_set_level(gpio_num_t(CONFIG_CS_GPIO), 1);

      /* We have finished accessing the shared resource.  Release the
            semaphore. */
      // xSemaphoreGiveFromISR(xSemaphore, &xHigherPriorityTaskWoken);
   }
   // else
   {
      /* We could not obtain the semaphore and can therefore not access
            the shared resource safely. */
   }

   // return rx_buf[1];
   return in[1];
}

/**
 * Perform hardware initialization.
 */
int lora_init(void)
{
   esp_err_t ret;

   /*
    * Configure CPU hardware to communicate with the radio chip
    */
   esp_rom_gpio_pad_select_gpio(gpio_num_t(CONFIG_RST_GPIO));
   gpio_set_direction(gpio_num_t(CONFIG_RST_GPIO), GPIO_MODE_OUTPUT);
   esp_rom_gpio_pad_select_gpio(gpio_num_t(CONFIG_CS_GPIO));
   gpio_set_direction(gpio_num_t(CONFIG_CS_GPIO), GPIO_MODE_OUTPUT);

   spi_bus_config_t bus{};
   bus.mosi_io_num = CONFIG_MOSI_GPIO;
   bus.miso_io_num = CONFIG_MISO_GPIO;
   bus.sclk_io_num = CONFIG_SCK_GPIO;
   bus.quadwp_io_num = -1;
   bus.quadhd_io_num = -1;
   bus.max_transfer_sz = 0;

   ret = spi_bus_initialize(VSPI_HOST, &bus, 0);
   assert(ret == ESP_OK);

   // spi_device_interface_config_t dev = {
   //     .command_bits = 0,
   //     .address_bits = 8,
   //     .dummy_bits = 0,
   //     .mode = 0, //
   //     .clock_speed_hz = 9000000,
   //     .spics_io_num = -1,
   //     .flags = 0,
   //     .queue_size = 1,
   //     .pre_cb = NULL};

   spi_device_interface_config_t dev{};
   dev.mode = 0;
   dev.clock_speed_hz = 9000000;
   dev.spics_io_num = -1;
   dev.flags = 0;
   dev.queue_size = 1;
   dev.pre_cb = NULL;

   ret = spi_bus_add_device(VSPI_HOST, &dev, &__spi);
   assert(ret == ESP_OK);

   /*
    * Perform hardware reset.
    */
   lora_reset();

   /*
    * Check version.
    */
   uint8_t version;
   uint8_t i = 0;
   while (i++ < TIMEOUT_RESET)
   {
      version = lora_read_reg(REG_VERSION);
      if (version == 0x12)
         break;
      // vTaskDelay(2);
      esphome::delay(2);

   }
   assert(i <= TIMEOUT_RESET + 1); // at the end of the loop above, the max value i can reach is TIMEOUT_RESET + 1

   /*
    * Default configuration.
    */
   lora_sleep();
   lora_write_reg(REG_FIFO_RX_BASE_ADDR, 0);
   lora_write_reg(REG_FIFO_TX_BASE_ADDR, 0);
   lora_write_reg(REG_LNA, lora_read_reg(REG_LNA) | 0x03);
   lora_write_reg(REG_MODEM_CONFIG_3, 0x04);
   lora_setTxPower(17);

   lora_setFrequency(433E6);

   lora_idle();
   return 1;
}

void lora_end()
{
   // put in sleep mode
   lora_sleep();

// stop SPI
#ifdef TODO
#pragma warning("Stop SPI to do")
   _spi->end();
#endif
}

int lora_beginPacket(int implicitHeader)
{
   if (lora_isTransmitting())
   {
      return 0;
   }

   // put in standby mode
   lora_idle();

   if (implicitHeader)
   {
      lora_implicitHeaderMode();
   }
   else
   {
      lora_explicitHeaderMode();
   }

   // reset FIFO address and paload length
   lora_write_reg(REG_FIFO_ADDR_PTR, 0);
   lora_write_reg(REG_PAYLOAD_LENGTH, 0);

   return 1;
}

int lora_endPacket(bool async)
{
   // BW: 7800 kHz, SF: 12, Preamble: 12 => Whole packet time: 16 935.4 ms
   if (async)
   {
      // lora_write_reg(REG_DIO_MAPPING_1, 0x40); // DIO0 => TXDONE
      lora_setInterruptMode(0 /* DIO0 */, LORA_IRQ_DIO0_TXDONE);
   }

   lora_tx();

   if (!async)
   {
      // wait for TX done
      while ((lora_read_reg(REG_IRQ_FLAGS) & LORA_IRQ_FLAG_TX_DONE) == 0)
      {
         // vTaskDelay(2);
         esphome::delay(2);
      }
      lora_clearInterrupts(LORA_IRQ_FLAG_TX_DONE);
   }

   return 1;
}

bool lora_isTransmitting()
{

   if ((lora_read_reg(REG_OP_MODE) & MODE_TX) == MODE_TX)
   {
      return true;
   }

   if (lora_read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK)
   {
      // clear IRQ's
      lora_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
   }

   return false;
}

int lora_parsePacket(int size)
{
   int payloadLength = 0;
   const uint8_t irqFlags = lora_readInterrupts();

   payloadLength = lora_parsePacket(irqFlags, size);

   return payloadLength;
}

int lora_parsePacket(uint8_t irqFlags, int size)
{
   int payloadLength = 0;

   if (size > 0)
   {
      lora_implicitHeaderMode();

      lora_write_reg(REG_PAYLOAD_LENGTH, size & 0xff);
   }
   else
   {
      lora_explicitHeaderMode();
   }

   if (irqFlags)
   {
      lora_clearInterrupts(irqFlags);
   }

   if ((irqFlags & LORA_IRQ_FLAG_RX_DONE) && (irqFlags & LORA_IRQ_FLAG_PAYLOAD_CRC_ERROR) == 0)
   {
      // received a packet
      _packetIndex = 0;

      // read payload length
      payloadLength = lora_getPayloadLength();

      // set FIFO address to current RX address
      lora_write_reg(REG_FIFO_ADDR_PTR, lora_read_reg(REG_FIFO_RX_CURRENT_ADDR));

      // put in standby mode
      lora_idle();
   }
   else if (lora_getDeviceMode() != DeviceMode::ReceiveSingle)
   {
      // reset FIFO address
      lora_write_reg(REG_FIFO_ADDR_PTR, 0);

      lora_rxSingle();
   }

   return payloadLength;
}

/**
 * Return last packet's RSSI.
 */
int lora_packetRssi(void)
{
   return (lora_read_reg(REG_PKT_RSSI_VALUE) - (__frequency < RF_MID_BAND_THRESHOLD ? RSSI_OFFSET_LF_PORT : RSSI_OFFSET_HF_PORT));
}

/**
 * Return last packet's SNR (signal to noise ratio).
 */
float lora_packetSnr(void)
{
   return ((int8_t)lora_read_reg(REG_PKT_SNR_VALUE)) * 0.25;
}

float lora_packetFrequencyError()
{
   int32_t freqError = 0;
   freqError = static_cast<int32_t>(lora_read_reg(REG_FREQ_ERROR_MSB) & 0b111);
   freqError <<= 8L;
   freqError += static_cast<int32_t>(lora_read_reg(REG_FREQ_ERROR_MID));
   freqError <<= 8L;
   freqError += static_cast<int32_t>(lora_read_reg(REG_FREQ_ERROR_LSB));

   if (lora_read_reg(REG_FREQ_ERROR_MSB) & 0b1000)
   {                       // Sign bit is on
      freqError -= 524288; // B1000'0000'0000'0000'0000
   }

   const float fXtal = 32E6;                                                                                              // FXOSC: crystal oscillator (XTAL) frequency (2.5. Chip Specification, p. 14)
   const float fError = ((static_cast<float>(freqError) * (1L << 24)) / fXtal) * (lora_getSignalBandwidth() / 500000.0f); // p. 37

   return fError;
}

int lora_rssi()
{
   return (lora_read_reg(REG_RSSI_VALUE) - (__frequency < RF_MID_BAND_THRESHOLD ? RSSI_OFFSET_LF_PORT : RSSI_OFFSET_HF_PORT));
}

void lora_compensateFrequencyOffset(const float &fError)
{
   const int8_t ppmOffset = static_cast<int8_t>(0.95f * (fError * 10E6f / __frequency));

   lora_setFrequency(static_cast<long>(__frequency - fError));
   lora_write_reg(REG_PPM_CORRECTION, reinterpret_cast<const uint8_t &>(ppmOffset));
}

size_t lora_write(const uint8_t *buffer, size_t size)
{
   int currentLength = lora_read_reg(REG_PAYLOAD_LENGTH);

   // check size
   if ((currentLength + size) > MAX_PKT_LENGTH)
   {
      size = MAX_PKT_LENGTH - currentLength;
   }

   // write data
   for (size_t i = 0; i < size; i++)
   {
      lora_write_reg(REG_FIFO, buffer[i]);
   }

   // update length
   lora_write_reg(REG_PAYLOAD_LENGTH, currentLength + size);

   return size;
}

size_t lora_write(uint8_t byte)
{
   return lora_write(const_cast<uint8_t *>(&byte), sizeof(byte));
}

int lora_available()
{
   return (lora_read_reg(REG_RX_NB_BYTES) - _packetIndex);
}

int lora_read()
{
   if (!lora_available())
   {
      return -1;
   }

   _packetIndex++;

   return lora_read_reg(REG_FIFO);
}

int lora_peek()
{
   if (!lora_available())
   {
      return -1;
   }

   // store current FIFO address
   int currentAddress = lora_read_reg(REG_FIFO_ADDR_PTR);

   // read
   uint8_t b = lora_read_reg(REG_FIFO);

   // restore FIFO address
   lora_write_reg(REG_FIFO_ADDR_PTR, currentAddress);

   return b;
}

/**
 * Sets the radio transceiver in receive mode.
 * Incoming packets will be received.
 */
void lora_receive(void)
{
   lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
}

void lora_receive(int size)
{

   lora_write_reg(REG_DIO_MAPPING_1, 0x00); // DIO0 => RXDONE

   if (size > 0)
   {
      lora_implicitHeaderMode();

      lora_write_reg(REG_PAYLOAD_LENGTH, size & 0xff);
   }
   else
   {
      lora_explicitHeaderMode();
   }

   lora_rxContinuous();
}

uint8_t lora_getPayloadLength()
{
   return __implicit ? lora_read_reg(REG_PAYLOAD_LENGTH) : lora_read_reg(REG_RX_NB_BYTES);
}

/**
 * Sets the radio transceiver in idle mode.
 * Must be used to change registers and access the FIFO.
 */
void lora_idle(void)
{
   lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
   // vTaskDelay(1);
   esphome::delay(1);
}

/**
 * Sets the radio transceiver in sleep mode.
 * Low power consumption and FIFO is lost.
 */
void lora_sleep(void)
{
   lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
   // vTaskDelay(1);
   esphome::delay(1);

}

void lora_cad()
{
   lora_idle();
   //lora_write_reg(REG_DIO_MAPPING_1, 0b10100000);
   lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_CAD);
   // vTaskDelay(1);
   esphome::delay(1);

   //delayMicroseconds(120); // IDLE -> CAD takes about ~120 µs
}

void lora_tx()
{
   lora_write_reg(REG_OP_MODE, LORA_MODE_LONG_RANGE_MODE | LORA_MODE_TX);
   //delayMicroseconds(220); // IDLE -> TX takes about ~220 µs
   // vTaskDelay(1);
   esphome::delay(1);

}

void lora_rxSingle()
{
   lora_write_reg(REG_OP_MODE, LORA_MODE_LONG_RANGE_MODE | LORA_MODE_RX_SINGLE);
   //delayMicroseconds(120); // IDLE -> RXSINGLE takes about ~120 µs
   // vTaskDelay(1);
   esphome::delay(1);

}

void lora_rxContinuous()
{
   lora_write_reg(REG_OP_MODE, LORA_MODE_LONG_RANGE_MODE | LORA_MODE_RX_CONTINUOUS);
   //delayMicroseconds(115); // IDLE -> RXCONTINUOUS takes about ~115 µs
   // vTaskDelay(1);
   esphome::delay(1);

}

/**
 * Configure power level for transmission
 * @param level 2-17, from least to most power
 */
void lora_setTxPower(int level)
{
   // RF9x module uses PA_BOOST pin
   if (level < 2)
      level = 2;
   else if (level > 17)
      level = 17;
   lora_write_reg(REG_PA_CONFIG, PA_BOOST | (level - 2));
}

void lora_setTxPower(int level, int outputPin)
{
   if (PA_OUTPUT_RFO_PIN == outputPin)
   {
      // RFO
      if (level < 0)
      {
         level = 0;
      }
      else if (level > 14)
      {
         level = 14;
      }

      lora_write_reg(REG_PA_CONFIG, 0x70 | level);
   }
   else
   {
      // PA BOOST
      if (level > 17)
      {
         if (level > 20)
         {
            level = 20;
         }

         // subtract 3 from level, so 18 - 20 maps to 15 - 17
         level -= 3;

         // High Power +20 dBm Operation (Semtech SX1276/77/78/79 5.4.3.)
         lora_write_reg(REG_PA_DAC, 0x87);
         lora_setOCP(140);
      }
      else
      {
         if (level < 2)
         {
            level = 2;
         }
         //Default value PA_HF/LF or +17dBm
         lora_write_reg(REG_PA_DAC, 0x84);
         lora_setOCP(100);
      }

      lora_write_reg(REG_PA_CONFIG, PA_BOOST | (level - 2));
   }
}

/**
 * Set carrier frequency.
 * @param frequency Frequency in Hz
 */
void lora_setFrequency(long frequency)
{
   __frequency = frequency;

   uint64_t frf = ((uint64_t)frequency << 19) / 32000000;

   lora_write_reg(REG_FRF_MSB, (uint8_t)(frf >> 16));
   lora_write_reg(REG_FRF_MID, (uint8_t)(frf >> 8));
   lora_write_reg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

int lora_getSpreadingFactor()
{
   int retval = 0;
   return (lora_read_reg(REG_MODEM_CONFIG_2) >> 4);
}

/**
 * Set spreading factor.
 * @param sf 6-12, Spreading factor to use.
 */
void lora_setSpreadingFactor(int sf)
{
   if (sf < 6)
      sf = 6;
   else if (sf > 12)
      sf = 12;

   if (sf == 6)
   {
      lora_write_reg(REG_DETECTION_OPTIMIZE, 0xc5);
      lora_write_reg(REG_DETECTION_THRESHOLD, 0x0c);
   }
   else
   {
      lora_write_reg(REG_DETECTION_OPTIMIZE, 0xc3);
      lora_write_reg(REG_DETECTION_THRESHOLD, 0x0a);
   }

   lora_write_reg(REG_MODEM_CONFIG_2, (lora_read_reg(REG_MODEM_CONFIG_2) & 0x0f) | ((sf << 4) & 0xf0));
   lora_setLdoFlag();
}

long lora_getSignalBandwidth()
{
   uint8_t bw = (lora_read_reg(REG_MODEM_CONFIG_1) >> 4);

   switch (bw)
   {
   case 0:
      return 7.8E3;
   case 1:
      return 10.4E3;
   case 2:
      return 15.6E3;
   case 3:
      return 20.8E3;
   case 4:
      return 31.25E3;
   case 5:
      return 41.7E3;
   case 6:
      return 62.5E3;
   case 7:
      return 125E3;
   case 8:
      return 250E3;
   case 9:
      return 500E3;
   }

   return -1;
}

/**
 * Set bandwidth (bit rate)
 * @param sbw Bandwidth in Hz (up to 500000)
 */
void lora_setSignalBandwidth(long sbw)
{
   int bw;

   if (sbw <= 7.8E3)
      bw = 0;
   else if (sbw <= 10.4E3)
      bw = 1;
   else if (sbw <= 15.6E3)
      bw = 2;
   else if (sbw <= 20.8E3)
      bw = 3;
   else if (sbw <= 31.25E3)
      bw = 4;
   else if (sbw <= 41.7E3)
      bw = 5;
   else if (sbw <= 62.5E3)
      bw = 6;
   else if (sbw <= 125E3)
      bw = 7;
   else if (sbw <= 250E3)
      bw = 8;
   else
      bw = 9;
   lora_write_reg(REG_MODEM_CONFIG_1, (lora_read_reg(REG_MODEM_CONFIG_1) & 0x0f) | (bw << 4));

   lora_setLdoFlag();
   if (bw == 9) {
      lora_write_reg(0x36, 0x02);
      lora_write_reg(0x3a, 0x7f);
   }
   
}

void lora_setLdoFlag()
{
   // ESP_LOGI(TAG, "Bandwidth %d:", uint8_t(lora_read_reg(REG_MODEM_CONFIG_1) >> 4));
   // ESP_LOGI(TAG, "Spreading %d:", uint8_t(lora_read_reg(REG_MODEM_CONFIG_2) >> 4));

   // ESP_LOGI(TAG, "Bandwidth %ld:", lora_getSignalBandwidth());
   // ESP_LOGI(TAG, "Spreading %d:",   lora_getSpreadingFactor());

   // Section 4.1.1.5
   long symbolDuration = 1000 / (lora_getSignalBandwidth() / (1L << lora_getSpreadingFactor()));

   // Section 4.1.1.6
   bool ldoOn = symbolDuration > 16;

   uint8_t config3 = lora_read_reg(REG_MODEM_CONFIG_3);
   bitWrite(config3, 3, ldoOn);
   lora_write_reg(REG_MODEM_CONFIG_3, config3);
}

/**
 * Set coding rate 
 * @param denominator 5-8, Denominator for the coding rate 4/x
 */
void lora_setCodingRate4(int denominator)
{
   if (denominator < 5)
      denominator = 5;
   else if (denominator > 8)
      denominator = 8;

   int cr = denominator - 4;
   lora_write_reg(REG_MODEM_CONFIG_1, (lora_read_reg(REG_MODEM_CONFIG_1) & 0xf1) | (cr << 1));
}

/**
 * Set the size of preamble.
 * @param length Preamble length in symbols.
 */
void lora_setPreambleLength(long length)
{
   lora_write_reg(REG_PREAMBLE_MSB, (uint8_t)(length >> 8));
   lora_write_reg(REG_PREAMBLE_LSB, (uint8_t)(length >> 0));
}

void lora_setSymbolTimeout(uint16_t symbols)
{
   if (symbols > 1023)
   { // p. 40
      symbols = 1023;
   }
   else if (symbols < 4)
   {
      symbols = 4;
   }

   const uint8_t currentMCValue = lora_read_reg(REG_MODEM_CONFIG_2) & 0b11111100;
   lora_write_reg(REG_MODEM_CONFIG_2, currentMCValue | ((uint8_t)(symbols >> 8)));
   lora_write_reg(REG_SYMB_TIMEOUT_LSB, (uint8_t)(symbols >> 0));
}

uint16_t lora_getSymbolTimeout()
{
   uint16_t status = (lora_read_reg(REG_MODEM_CONFIG_2) & 0b00000011);
   status <<= 8;
   status |= lora_read_reg(REG_SYMB_TIMEOUT_LSB);
   return status;
}

/**
 * Change radio sync word.
 * @param sw New sync word to use.
 */
void lora_setSyncWord(int sw)
{
   lora_write_reg(REG_SYNC_WORD, sw);
}

void lora_setMaxPayloadLength(const uint8_t payloadLength)
{
   lora_write_reg(REG_MAX_PAYLOAD_LENGTH, payloadLength);
}

void lora_enableLowDataRateOptimize(bool enabled)
{
   uint8_t regValue = lora_read_reg(REG_MODEM_CONFIG_3);
   lora_write_reg(REG_MODEM_CONFIG_3, bitWrite(regValue, 3, enabled));
}

void lora_enableTcxo(const bool enabled)
{
   uint8_t regValue = lora_read_reg(REG_TCXO);
   lora_write_reg(REG_TCXO, bitWrite(regValue, 4, enabled));
}

/**
 * Enable appending/verifying packet CRC.
 */
void lora_enableCrc(void)
{

   lora_write_reg(REG_MODEM_CONFIG_2, lora_read_reg(REG_MODEM_CONFIG_2) | 0x04);
}

/**
 * Disable appending/verifying packet CRC.
 */
void lora_disableCrc(void)
{
   lora_write_reg(REG_MODEM_CONFIG_2, lora_read_reg(REG_MODEM_CONFIG_2) & 0xfb);
}

void lora_enableInvertIQ()
{
   lora_write_reg(REG_INVERTIQ, 0x66);
   lora_write_reg(REG_INVERTIQ2, 0x19);
}

void lora_disableInvertIQ()
{
   lora_write_reg(REG_INVERTIQ, 0x27);
   lora_write_reg(REG_INVERTIQ2, 0x1d);
}

void lora_setOCP(uint8_t mA)
{
   uint8_t ocpTrim = 27;

   if (mA <= 120)
   {
      ocpTrim = (mA - 45) / 5;
   }
   else if (mA <= 240)
   {
      ocpTrim = (mA + 30) / 10;
   }

   lora_write_reg(REG_OCP, 0x20 | (0x1F & ocpTrim));
}

void lora_setGain(uint8_t gain)
{
   // check allowed range
   if (gain > 6)
   {
      gain = 6;
   }

   // set to standby
   lora_idle();

   // set gain
   if (gain == 0)
   {
      // if gain = 0, enable AGC
      lora_write_reg(REG_MODEM_CONFIG_3, 0x04);
   }
   else
   {
      // disable AGC
      lora_write_reg(REG_MODEM_CONFIG_3, 0x00);

      // clear Gain and set LNA boost
      lora_write_reg(REG_LNA, 0x03);

      // set gain
      lora_write_reg(REG_LNA, lora_read_reg(REG_LNA) | (gain << 5));
   }
}

void lora_setInterruptMode(uint8_t pin, uint8_t mode)
{
   if (pin <= 3)
   {
      uint8_t mapping = lora_read_reg(REG_DIO_MAPPING_1);
      bitWrite(mapping, 6 - (pin * 2), bitRead(mode, 0));
      bitWrite(mapping, 6 - (pin * 2) + 1, bitRead(mode, 1));
      lora_write_reg(REG_DIO_MAPPING_1, mapping);
   }
   else if (pin <= 5)
   {
      uint8_t mapping = lora_read_reg(REG_DIO_MAPPING_2);
      bitWrite(mapping, 14 - (pin * 2), bitRead(mode, 0));
      bitWrite(mapping, 14 - (pin * 2) + 1, bitRead(mode, 1));
      lora_write_reg(REG_DIO_MAPPING_2, mapping);
   }
}

uint8_t lora_readInterrupts()
{
   return lora_read_reg(REG_IRQ_FLAGS);
}

void lora_clearInterrupts(uint8_t irqFlags)
{
   lora_write_reg(REG_IRQ_FLAGS, irqFlags);
}

uint8_t lora_random()
{
   return lora_read_reg(REG_RSSI_WIDEBAND);
}

void lora_dump_registers(void)
{
   int i;
   printf("00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
   for (i = 0; i < 0x40; i++)
   {
      printf("%02X ", lora_read_reg(i));
      if ((i & 0x0f) == 0x0f)
         printf("\n");
   }
   printf("\n");
}

uint8_t lora_getDeviceMode()
{
   uint8_t mode = lora_read_reg(REG_OP_MODE) & 0b111;
   return mode;
}

/**
 * Configure explicit header mode.
 * Packet size will be included in the frame.
 */
void lora_explicitHeaderMode(void)
{
   __implicit = 0;
   lora_write_reg(REG_MODEM_CONFIG_1, lora_read_reg(REG_MODEM_CONFIG_1) & 0xfe);
}

/**
 * Configure implicit header mode.
 * All packets will have a predefined size.
 * @param size Size of the packets.
 */
void lora_implicitHeaderMode(int size)
{
   __implicit = 1;
   lora_write_reg(REG_MODEM_CONFIG_1, lora_read_reg(REG_MODEM_CONFIG_1) | 0x01);
   lora_write_reg(REG_PAYLOAD_LENGTH, size);
}

/**
 * Configure implicit header mode.
 * All packets will have a predefined size.
 * @param size Size of the packets.
 */
void lora_implicitHeaderMode()
{
   __implicit = 1;
   lora_write_reg(REG_MODEM_CONFIG_1, lora_read_reg(REG_MODEM_CONFIG_1) | 0x01);
   //lora_write_reg(REG_PAYLOAD_LENGTH, size);
}

// void lora_handleDio0RiseRx()
// {
//   const uint8_t irqFlags = lora_readInterrupts();

//   if (irqFlags) {
//     lora_clearInterrupts(irqFlags);
//   }

//   if ((irqFlags & LORA_IRQ_FLAG_PAYLOAD_CRC_ERROR) == 0) {

//     if ((irqFlags & LORA_IRQ_FLAG_RX_DONE) != 0) {
//       // received a packet
//       _packetIndex = 0;

//       // read packet length
//     int packetLength = lora_getPayloadLength();

//       // set FIFO address to current RX address
//       lora_write_reg(REG_FIFO_ADDR_PTR, lora_read_reg(REG_FIFO_RX_CURRENT_ADDR));

//       if (_onReceive) {
//         _onReceive(packetLength);
//       }
//     }
//     else if ((irqFlags & LORA_IRQ_FLAG_TX_DONE) != 0) {
//       if (_onTxDone) {
//         _onTxDone();
//       }
//     }
//   }
// }

/**
 * Perform physical reset on the Lora chip
 */
void lora_reset(void)
{
   gpio_set_level(gpio_num_t(CONFIG_RST_GPIO), 0);
   // vTaskDelay(pdMS_TO_TICKS(1));
   esphome::delay(1);

   gpio_set_level(gpio_num_t(CONFIG_RST_GPIO), 1);
   // vTaskDelay(pdMS_TO_TICKS(1));
   esphome::delay(1);

}

/**
 * Send a packet.
 * @param buf Data to be sent
 * @param size Size of data.
 */
void lora_send_packet(uint8_t *buf, int size)
{
   /*
    * Transfer data to radio.
    */
   lora_idle();
   lora_write_reg(REG_FIFO_ADDR_PTR, 0);

   for (int i = 0; i < size; i++)
      lora_write_reg(REG_FIFO, *buf++);

   lora_write_reg(REG_PAYLOAD_LENGTH, size);

   /*
    * Start transmission and wait for conclusion.
    */
   lora_write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
   while ((lora_read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0)
   {
      // vTaskDelay(2);
      esphome::delay(2);
   }
   lora_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
}

/**
 * Read a received packet.
 * @param buf Buffer for the data.
 * @param size Available size in buffer (bytes).
 * @return Number of bytes received (zero if no packet available).
 */
int lora_receive_packet(uint8_t *buf, int size)
{
   int len = 0;

   /*
    * Check interrupts.
    */
   int irq = lora_read_reg(REG_IRQ_FLAGS);
   lora_write_reg(REG_IRQ_FLAGS, irq);
   if ((irq & IRQ_RX_DONE_MASK) == 0)
      return 0;
   if (irq & IRQ_PAYLOAD_CRC_ERROR_MASK)
      return 0;

   /*
    * Find packet size.
    */
   if (__implicit)
      len = lora_read_reg(REG_PAYLOAD_LENGTH);
   else
      len = lora_read_reg(REG_RX_NB_BYTES);

   /*
    * Transfer data from radio.
    */
   lora_idle();
   lora_write_reg(REG_FIFO_ADDR_PTR, lora_read_reg(REG_FIFO_RX_CURRENT_ADDR));
   if (len > size)
      len = size;
   for (int i = 0; i < len; i++)
      *buf++ = lora_read_reg(REG_FIFO);

   return len;
}

/**
 * Returns non-zero if there is data to read (packet received).
 */
int lora_received(void)
{
   if (lora_read_reg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK)
      return 1;
   return 0;
}

/**
 * Shutdown hardware.
 */
void lora_close(void)
{
   lora_sleep();
   //   close(__spi);  FIXME: end hardware features after lora_close
   //   close(__cs);
   //   close(__rst);
   //   __spi = -1;
   //   __cs = -1;
   //   __rst = -1;
}
