#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include <esp_bt_defs.h> //For esp_bd_addr_t

#include <array>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"

// Configuration
#define POOL_SIZE 5
#define BUFFER_SIZE 256
#define UART_NUM UART_NUM_1
#define RX_QUEUE_SIZE 20
#define MAGIC_NUMBER 0xDEADBEEF

// // Memory pool structure
// typedef struct
// {
//   uint8_t data[BUFFER_SIZE];
//   size_t length;
//   uint32_t timestamp;
// } rx_buffer_t;
// Memory pool structure with safety features
typedef struct
{
  uint8_t data[BUFFER_SIZE];
  size_t length;
  uint32_t timestamp;
  uint32_t magic;    // Magic number for validation
  uint8_t in_use;    // Flag to track buffer state
  uint8_t ref_count; // Reference counting
} rx_buffer_t;

// Statistics
typedef struct
{
  uint32_t buffers_allocated;
  uint32_t buffers_freed;
  uint32_t allocation_failures;
  uint32_t double_free_attempts;
  uint32_t invalid_buffer_errors;
} pool_stats_t;

namespace esphome
{
  namespace lora_tracker
  {

    // class LORATracker;

    // class LORAClientNode
    // {
    // public:
    //   virtual void set_response(uint8_t *data, size_t len) = 0;

    //   virtual void loop() {}
    //   void set_lora_client_parent(LORAClient *parent) { this->parent_ = parent; }

    // protected:
    //   LORAClient *parent_{nullptr};
    // };

    class LORAListener;
    class LORAClient;


    class LORATracker : public Component
    {
    public:
      LORATracker();

      static const int loraSpreadingFactor = 7;
      static const int loraCodingRate = 8;
      static const int loraPreambleLengthRx = 8;
      static const int loraPreambleLengthTx = 8;
      static const long loraSignalBandwidth = 500e3;
      static const int loraSyncWord = 0x12;
      static const uint64_t loraPollingTimeout = 75;

      static const uint8_t broadcastAddressing = 0xFF;
      static const uint8_t subnetAddressing = 0xFE;

      /// Setup the FreeRTOS task and the Bluetooth stack.
      void setup() override;
      void dump_config() override;
      float get_setup_priority() const override;

      void loop() override;
      void receive();
      void checkReception();
      void send(uint8_t *data, size_t len);
      void sendPacketOnce(uint8_t *data, size_t len);
      void sendPacketBurst(uint8_t *data, size_t len);
      void sendPacketBytes(uint8_t *data, size_t len);
      void sendTask(void *pvParameters);
      void register_listener(LORAListener *listener);
      void register_client(LORAClient *client);

      // F-11: link-quality of the most recently received packet.  Updated in
      // checkReception() before the packet is dispatched, so a client that
      // accepts the packet can read the value that belongs to it.
      int get_last_rssi() const { return this->last_packet_rssi_; }
      float get_last_snr() const { return this->last_packet_snr_; }

    protected:
      int   last_packet_rssi_{0};
      float last_packet_snr_{0.0f};

      esp_err_t init_memory_pool(void);
      rx_buffer_t *get_free_buffer(TickType_t timeout);
      esp_err_t return_buffer_to_pool(rx_buffer_t *buffer);
      esp_err_t buffer_ref_inc(rx_buffer_t *buffer);
      bool validate_buffer(rx_buffer_t *buffer);

      // Group 1: Large objects (12+ bytes) - vectors and callback manager
      std::vector<LORAListener *> listeners_;
      std::vector<LORAClient *> clients_;
      // Group 4: 1-byte types (enums, uint8_t, bool)
      uint8_t app_id_{0};

      // Serialises all SX1278 SPI access so a TX copy and an RX read can never
      // overlap on the shared bus.  lora_tx_busy_ is kept only as a lightweight
      // hint that lets the main loop skip receive() while a copy is actively
      // transmitting (avoiding needless blocking on the mutex); the mutex is the
      // actual mutual-exclusion guarantee between sendTask and the main loop.
      SemaphoreHandle_t radio_mutex_{nullptr};
      bool lora_tx_busy_{false};
      uint8_t buf_[255]; // Maximum Payload size of SX1276/77/78/79 is 255

      // rx_buffer_t memory_pool[POOL_SIZE] {};
      // QueueHandle_t free_buffer_queue;
      // QueueHandle_t data_queue;
      // SemaphoreHandle_t pool_mutex;
      // pool_stats_t pool_stats = {};

      int rxSlotsPerRound{3};
      int txSlotsPerRound{17};
      int roundDurationMs{1500};
      // Quiet window held in RX after each burst so the addressed node can send
      // its deferred reply (ACK/position) without being stepped on by the next
      // burst.  Sized for the node's estimate error + pre-CAD backoff + CAD +
      // ~60-byte airtime (the node can take a few hundred ms after burst-end to
      // actually get the reply on air).
      int responseWindowMs{400};
      int slotDurationMs{roundDurationMs / (rxSlotsPerRound * txSlotsPerRound)};
      int rxIntervalMs{roundDurationMs / rxSlotsPerRound};
      int txIntervalMs{roundDurationMs / txSlotsPerRound};

    public:
    };

    // NOLINTNEXTLINE
    extern LORATracker *volatile global_lora_tracker;

  } // namespace esphome::lora_tracker
}
