

#include "lora_tracker.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#define CONFIG_CS_GPIO 19
#define CONFIG_RST_GPIO 16
#define CONFIG_MISO_GPIO 5
#define CONFIG_MOSI_GPIO 18
#define CONFIG_SCK_GPIO 17

#include "lora.h"

#include <freertos/FreeRTOS.h>
#include <freertos/FreeRTOSConfig.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <cinttypes>

#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

#undef TAG

static rx_buffer_t memory_pool[POOL_SIZE];
static QueueHandle_t free_buffer_queue;
static QueueHandle_t data_queue;
static SemaphoreHandle_t pool_mutex;
static pool_stats_t pool_stats = {0};

namespace esphome
{
  namespace lora_tracker
  {

    static const char *const TAG = "lora_tracker";

    LORATracker *volatile global_lora_tracker = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    void sendTaskDispatcher(void *pvParameters)
    {
      (void)pvParameters;
      ESP_LOGI(TAG, "sendTaskDispatcher started, waiting for global_lora_tracker initialization");

      // Wait for global_lora_tracker to be initialized with timeout
      int max_wait_ms = 5000; // 5 second timeout
      int wait_iterations = 0;

      while (global_lora_tracker == nullptr && wait_iterations < (max_wait_ms / 10))
      {
        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
        wait_iterations++;
      }

      if (global_lora_tracker == nullptr)
      {
        ESP_LOGE(TAG, "sendTaskDispatcher timeout: global_lora_tracker is still nullptr after %d ms", max_wait_ms);
        vTaskDelete(NULL); // Delete this task
        return;
      }

      ESP_LOGI(TAG, "sendTaskDispatcher: global_lora_tracker initialized at %p, calling sendTask", global_lora_tracker);
      global_lora_tracker->sendTask(pvParameters);

      // Task should not reach here unless sendTask returns
      ESP_LOGW(TAG, "sendTaskDispatcher: sendTask returned unexpectedly");
      vTaskDelete(NULL);
    }

    void spawn_send_task()
    {
      // xTaskCreatePinnedToCore(
      //     sendTaskDispatcher,
      //     "lora_send_task", // name
      //     4096,             // stack size
      //     nullptr,          // task pv params
      //     1,                // priority
      //     nullptr,          // handle
      //     1                 // core
      // );

      // xTaskCreate(sendTaskDispatcher, "sendTaskDispatcher", 4096, NULL, 2, NULL);
      BaseType_t ret = xTaskCreatePinnedToCore(sendTaskDispatcher,
                                               "sendTaskDispatcher", // A name just for humans
                                               4096,                 // This stack size can be checked & adjusted by reading the Stack Highwater
                                               NULL,
                                               1, // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest.
                                               NULL,
                                               1); // Pin task to core 1
      if (ret != pdPASS)
      {
        ESP_LOGE(TAG, "Failed to create sendTaskDispatcher task");
      }
      else
      {
        ESP_LOGI(TAG, "sendTaskDispatcher task created successfully");
      }
    }


    LORATracker::LORATracker()
        : Component()
    // ,free_buffer_queue(nullptr),
    // data_queue(nullptr),
    // pool_mutex(nullptr),
    // pool_stats({0})
    {
      ESP_ERROR_CHECK(this->init_memory_pool());
      ESP_LOGI(TAG, "LORATracker memory pool initialized");

      // Created here (before setup()/sendTask) so it is always valid by the time
      // either the main loop or the send task touches the radio.
      this->radio_mutex_ = xSemaphoreCreateMutex();
      if (this->radio_mutex_ == nullptr)
      {
        ESP_LOGE(TAG, "Failed to create radio mutex");
      }
    }

    float LORATracker::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

    void LORATracker::dump_config()
    {
      ESP_LOGCONFIG(TAG, "LORA Tracker:");
    }

    void LORATracker::setup()
    {
      global_lora_tracker = this;

      ESP_LOGI(TAG, "LORATracker setup started");
      // ESP_ERROR_CHECK(this->init_memory_pool());

      lora_init();

      lora_setFrequency(433.05E6 + 250e3);
      lora_setSpreadingFactor(loraSpreadingFactor);
      lora_setCodingRate4(loraCodingRate);
      lora_setPreambleLength(loraPreambleLengthRx);
      lora_setSignalBandwidth(loraSignalBandwidth);
      lora_setSyncWord(loraSyncWord);

      lora_setTxPower(8, PA_OUTPUT_RFO_PIN);

      lora_enableCrc();

      // For other constants, like FHSS change channel, CRC error, or RX timeout, see the LoRa.h header file.
      // Choose from LORA_IRQ_DIOx_ variants and use this "x" number in place of the first parameter.
      // Not all DIOx and interrupt type mixes are possible.

      // lora_clearInterrupts(LORA_IRQ_FLAG_CAD_DETECTED | LORA_IRQ_FLAG_CAD_DONE);
      lora_clearInterrupts(0xff);
      lora_setInterruptMode(0 /* DIO0 */, LORA_IRQ_DIO0_CADDONE);
      lora_setInterruptMode(1 /* DIO1 */, LORA_IRQ_DIO1_RXTIMEOUT);

      spawn_send_task();
    }

    void LORATracker::loop()
    {
      if (lora_tx_busy_ == false)
        this->receive();
      esphome::delay(10);
      // this->sendPacketBytes(txBuf, len);

      // rx_buffer_t *rx_buffer;

      // ESP_LOGI(TAG, "Data processing task started");

      // while (1)
      // {
      // if (xQueueReceive(data_queue, &rx_buffer, 0) == pdTRUE)
      // {
      //   // Validate before processing
      //   if (!this->validate_buffer(rx_buffer))
      //   {
      //     ESP_LOGE(TAG, "Received invalid buffer, skipping");
      //     return;
      //   }

      //   // Additional bounds check
      //   if (rx_buffer->length > BUFFER_SIZE)
      //   {
      //     ESP_LOGE(TAG, "Buffer length %d exceeds max %d",
      //              rx_buffer->length, BUFFER_SIZE);
      //     this->return_buffer_to_pool(rx_buffer);
      //     return;
      //   }

      //   ESP_LOGI(TAG, "Processing %d bytes from buffer %p",
      //            rx_buffer->length, rx_buffer);

      //   // Simulate processing
      //   // vTaskDelay(50 / portTICK_PERIOD_MS);
      //   this->sendPacketBurst(rx_buffer->data, rx_buffer->length);
      //   // Safe hex dump with length check
      //   if (rx_buffer->length > 0)
      //   {
      //     ESP_LOG_BUFFER_HEX(TAG, rx_buffer->data, rx_buffer->length);
      //   }

      //   // Return buffer to pool
      //   this->return_buffer_to_pool(rx_buffer);
      // }
      // }
    }

    void LORATracker::checkReception()
    {
      // Read the FIFO + link quality under the radio mutex so the SPI burst can
      // never interleave with a TX copy on the send task.  Dispatch happens
      // AFTER releasing the mutex: set_response() does crypto, NVS writes and
      // scheduler work that must not block the radio (and only ever runs on the
      // main-loop task, so buf_ has no other reader).
      if (this->radio_mutex_ != nullptr)
        xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);

      int rxLen = lora_receive_packet(buf_, sizeof(buf_));
      if (rxLen > 0)
      {
        // F-11: cache link quality for this packet before dispatch so clients
        // can publish it to Home Assistant.
        this->last_packet_rssi_ = lora_packetRssi();
        this->last_packet_snr_  = lora_packetSnr();
      }

      if (this->radio_mutex_ != nullptr)
        xSemaphoreGive(this->radio_mutex_);

      if (rxLen > 0)
      {
        ESP_LOGI(TAG, "Received packet with length %d", rxLen);
        float packetStrength = this->last_packet_rssi_ + this->last_packet_snr_ * 0.25;

        ESP_LOGI(TAG, "Packet RSSI: %d, SNR: %.2f, Estimated Strength: %.2f dBm",
                 this->last_packet_rssi_, this->last_packet_snr_, packetStrength);

        // ESP_LOGI(TAG, "%s byte packet received:[%.*s]", rxLen, rxLen, buf_);
        for (int i = 0; i < this->clients_.size(); i++)
        {
          clients_[i]->set_response(buf_, rxLen);
        }
      }
    }

    void LORATracker::sendTask(void *pvParameters)
    {
      (void)pvParameters;

      rx_buffer_t *rx_buffer;

      for (;;)
      {
        if (xQueueReceive(data_queue, &rx_buffer, portMAX_DELAY) == pdTRUE)
        {

          // Validate before processing
          if (!this->validate_buffer(rx_buffer))
          {
            ESP_LOGE(TAG, "Received invalid buffer, skipping");
            return;
          }

          // Additional bounds check
          if (rx_buffer->length > BUFFER_SIZE)
          {
            ESP_LOGE(TAG, "Buffer length %d exceeds max %d",
                     rx_buffer->length, BUFFER_SIZE);
            this->return_buffer_to_pool(rx_buffer);
            return;
          }

          ESP_LOGI(TAG, "Processing %d bytes from buffer %p",
                   rx_buffer->length, rx_buffer);

          // Simulate processing
          // vTaskDelay(50 / portTICK_PERIOD_MS);
          lora_tx_busy_ = true;
          this->sendPacketBurst(rx_buffer->data, rx_buffer->length);
          lora_tx_busy_ = false;

          // Safe hex dump with length check
          if (rx_buffer->length > 0)
          {
            ESP_LOG_BUFFER_HEX(TAG, rx_buffer->data, rx_buffer->length);
          }

          // Return buffer to pool
          this->return_buffer_to_pool(rx_buffer);

          // Post-burst response window.  The addressed node defers its reply
          // (ACK / position) until just after this burst ends, then transmits
          // into the clear channel.  Hold off dequeuing the next burst and keep
          // the radio in RX so that reply is heard instead of being stepped on
          // by the next burst.  lora_tx_busy_ is already false, so the main
          // loop's receive() reads any incoming packet during this window.
          if (this->radio_mutex_ != nullptr)
            xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);
          lora_receive(0);
          if (this->radio_mutex_ != nullptr)
            xSemaphoreGive(this->radio_mutex_);
          vTaskDelay(pdMS_TO_TICKS(this->responseWindowMs));
        }
      }
    }

    void LORATracker::receive()
    {
      // lora_receive(0);

      // Read the mode and (re)arm RX under the mutex.  checkReception() takes
      // the mutex itself, so call it AFTER releasing here to avoid re-entrancy
      // on the non-recursive mutex.
      if (this->radio_mutex_ != nullptr)
        xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);
      uint8_t mode = lora_getDeviceMode();
      if (mode != DeviceMode::ReceiveSingle &&
          mode != DeviceMode::ReceiveContinous &&
          mode != DeviceMode::Transmit)
      {
        lora_receive(0); // put into receive mode
      }
      if (this->radio_mutex_ != nullptr)
        xSemaphoreGive(this->radio_mutex_);

      if (mode == DeviceMode::ReceiveSingle || mode == DeviceMode::ReceiveContinous)
      {
        checkReception();
      }

      // ESP_LOGI(TAG, ". : %d", mode);
    }

    void LORATracker::send(uint8_t *data, size_t len)
    {
      // Validate inputs first
      if (!data || len == 0)
      {
        ESP_LOGW(TAG, "Invalid send parameters: data=%p, len=%zu", data, len);
        return;
      }

      // Check queue initialization
      if (free_buffer_queue == nullptr || data_queue == nullptr)
      {
        ESP_LOGE(TAG, "Queues not initialized. free_buffer_queue=%p, data_queue=%p",
                 free_buffer_queue, data_queue);
        return;
      }
      // if (free_buffer_queue == NULL)
      // {
      //   ESP_LOGE(TAG, "Free buffer queue not initialized");
      //   ESP_ERROR_CHECK(this->init_memory_pool());
      // }

      UBaseType_t free_buffers = uxQueueMessagesWaiting(free_buffer_queue);
      ESP_LOGI(TAG, "Free buffers available: %d", free_buffers);

      // Ensure we don't overflow
      if (len >= BUFFER_SIZE)
      {
        ESP_LOGW(TAG, "Truncating %d bytes to %d", len, BUFFER_SIZE - 1);
        len = BUFFER_SIZE - 1;
      }

      // Get a free buffer from pool
      rx_buffer_t *rx_buffer = this->get_free_buffer(10 / portTICK_PERIOD_MS);

      if (rx_buffer)
      {
        // Safe copy with explicit size limit
        size_t copy_len = (len < BUFFER_SIZE) ? len : BUFFER_SIZE - 1;
        memcpy(rx_buffer->data, data, copy_len);
        rx_buffer->length = copy_len;
        rx_buffer->data[copy_len] = '\0'; // Null terminate for safety

        // Send buffer pointer to data queue
        if (xQueueSend(data_queue, &rx_buffer, 0) != pdTRUE)
        {
          ESP_LOGW(TAG, "Data queue full, dropping data");
          this->return_buffer_to_pool(rx_buffer);
        }
      }
      else
      {
        ESP_LOGW(TAG, "No free buffers, dropped %d bytes", len);
      }
    }

    void LORATracker::sendPacketBurst(uint8_t *data, size_t len)
    {

      // const TickType_t xFrequency = pdMS_TO_TICKS(142); // For RX /TX config 3x RX + 7x TX
      // const TickType_t xFrequency = pdMS_TO_TICKS(59); // For RX /TX config 3x RX + 17x TX in 1sec
      const TickType_t xFrequency = pdMS_TO_TICKS(this->txIntervalMs); // For RX /TX config 3x RX + 17x TX in 1.5sec

      TickType_t xLastWakeTime;
      ESP_LOGI(TAG, "Sending packed burst");
      xLastWakeTime = xTaskGetTickCount();

      // Burst indexing: re-stamp header.burstIndex on every copy (and set
      // burstCount) so the node can compute when the burst ends and defer its
      // reply into the clear window afterwards.  Unpack once; each copy is
      // re-packed with its own index.  On unpack failure (should not happen for
      // our own freshly-packed frames) fall back to sending the raw bytes.
      LoraClientOperationMessage *burstMsg =
          lora_client_operation_message__unpack(NULL, len, data);
      const bool canStamp = (burstMsg != nullptr && burstMsg->header != nullptr);
      if (canStamp)
        burstMsg->header->burstcount = this->txSlotsPerRound;

      // for (int cnt = 0; cnt < 7; cnt++)
      for (int cnt = 0; cnt < this->txSlotsPerRound; cnt++)
      {
        this->lora_tx_busy_ = true;

        if (canStamp)
        {
          burstMsg->header->burstindex = cnt;
          size_t clen  = lora_client_operation_message__get_packed_size(burstMsg);
          uint8_t *cbuf = static_cast<uint8_t *>(malloc(clen));
          if (cbuf)
          {
            lora_client_operation_message__pack(burstMsg, cbuf);
            this->sendPacketBytes(cbuf, clen);
            free(cbuf);
          }
          else
          {
            this->sendPacketBytes(data, len); // OOM fallback: unindexed copy
          }
        }
        else
        {
          this->sendPacketBytes(data, len);
        }
        // sendPacketBytes((uint8_t *)&curTime, sizeof(curTime));

        // Release the radio to RX during the idle gap between copies.  Holding
        // it TX-busy for the whole 17-copy burst (~1.5 s) kept the hub deaf:
        // it never opened an RX window to hear the node's CommandAck, and the
        // node's CAD saw a permanently busy channel and could not transmit the
        // ACK either — so every cover op retransmitted to 4/4.  Clearing
        // lora_tx_busy_ here lets the main loop's receive() read any incoming
        // reply (in the main task, where the scheduler/NVS/publish are safe)
        // during the gap.  No gap after the final copy — the caller clears
        // lora_tx_busy_ and the loop returns to RX immediately.
        if (cnt + 1 < this->txSlotsPerRound)
        {
          // Arm RX under the mutex, then drop the busy hint so the main loop can
          // read the FIFO during the gap.  The mutex (not the flag) guarantees
          // the next sendPacketBytes() cannot start until any in-progress read
          // on the main loop has finished.
          if (this->radio_mutex_ != nullptr)
            xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);
          lora_receive(0);             // continuous RX for the duration of the gap
          if (this->radio_mutex_ != nullptr)
            xSemaphoreGive(this->radio_mutex_);

          this->lora_tx_busy_ = false; // allow loop()->receive() to read the FIFO
          vTaskDelayUntil(&xLastWakeTime, xFrequency);
        }
      }

      if (burstMsg != nullptr)
        lora_client_operation_message__free_unpacked(burstMsg, NULL);
    }

    void LORATracker::sendPacketOnce(uint8_t *data, size_t len)
    {

      this->sendPacketBytes(data, len);
    }

    void LORATracker::sendPacketBytes(uint8_t *data, size_t len)
    {

      // vTaskSuspend(xHandleLoraPolling);
      // Hold the radio mutex for the whole transmit so a concurrent RX read on
      // the main loop cannot interleave SPI transactions with the TX sequence.
      if (this->radio_mutex_ != nullptr)
        xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);

      ESP_LOGI(TAG, "Sending packet of length %d", len);
      lora_idle();

      lora_setSpreadingFactor(loraSpreadingFactor);
      lora_setCodingRate4(loraCodingRate);
      lora_setPreambleLength(loraPreambleLengthTx);
      lora_setSignalBandwidth(loraSignalBandwidth);
      lora_setSyncWord(loraSyncWord);
      lora_enableCrc();
      // lora_setSymbolTimeout(1023);

      int status = lora_beginPacket(); // start packet
      if (status == 0)
      {
        ESP_LOGW(TAG, "Already Xmitting");
      }

      lora_write(data, len); // add destination address

      // bool async = true;
      bool async = false;

      if (async)
      {
      }

      status = lora_endPacket(async); // finish packet and send it
      if (status == 0)
      {
        ESP_LOGW(TAG, "TX timeout");
      }

      if (!async)
      {
        ESP_LOGI(TAG, "Packet sent");
        lora_setPreambleLength(loraPreambleLengthRx);

        // lora_sleep();

        // vTaskResume(xHandleLoraPolling);
      }
      else
      {
        ESP_LOGI(TAG, "Packet sent, waiting for TX DONE interrupt");
      }

      if (this->radio_mutex_ != nullptr)
        xSemaphoreGive(this->radio_mutex_);
    }

    void LORATracker::register_client(LORAClient *client)
    {
      client->set_parent(this);          // required: lets the client send via the radio
      client->app_id = ++this->app_id_;
      this->clients_.push_back(client);
    }

    void LORATracker::register_listener(LORAListener *listener)
    {
      listener->set_parent(this);
      this->listeners_.push_back(listener);
    }

    // Initialize memory pool and queues
    esp_err_t LORATracker::init_memory_pool(void)
    {
      // Create queues - use queue registry for debugging
      free_buffer_queue = xQueueCreate(POOL_SIZE, sizeof(rx_buffer_t *));
      data_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(rx_buffer_t *));

      if (!free_buffer_queue || !data_queue)
      {
        ESP_LOGE(TAG, "Failed to create queues");
        return ESP_FAIL;
      }

      vQueueAddToRegistry(free_buffer_queue, "FreeBuffers");
      vQueueAddToRegistry(data_queue, "DataQueue");

      // Create mutex for pool protection
      pool_mutex = xSemaphoreCreateMutex();
      if (!pool_mutex)
      {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
      }

      // Initialize all buffers in pool
      for (int i = 0; i < POOL_SIZE; i++)
      {
        memset(&memory_pool[i], 0, sizeof(rx_buffer_t));
        memory_pool[i].magic = MAGIC_NUMBER;
        memory_pool[i].in_use = 0;
        memory_pool[i].ref_count = 0;

        rx_buffer_t *buffer = &memory_pool[i];
        if (xQueueSend(free_buffer_queue, &buffer, 0) != pdTRUE)
        {
          ESP_LOGE(TAG, "Failed to initialize free buffer queue");
          return ESP_FAIL;
        }
      }

      ESP_LOGI(TAG, "Memory pool initialized with %d buffers", POOL_SIZE);
      return ESP_OK;
    }

    // Get a free buffer from the pool (thread-safe)
    rx_buffer_t *LORATracker::get_free_buffer(TickType_t timeout)
    {
      rx_buffer_t *buffer = NULL;

      if (xQueueReceive(free_buffer_queue, &buffer, timeout) != pdTRUE)
      {
        pool_stats.allocation_failures++;
        return NULL;
      }

      // Take mutex to safely modify buffer state
      if (xSemaphoreTake(pool_mutex, portMAX_DELAY) == pdTRUE)
      {
        // Validate buffer
        if (!this->validate_buffer(buffer))
        {
          xSemaphoreGive(pool_mutex);
          return NULL;
        }

        // Check if buffer is already in use (should never happen)
        if (buffer->in_use)
        {
          ESP_LOGE(TAG, "Buffer %p already in use!", buffer);
          xSemaphoreGive(pool_mutex);
          pool_stats.invalid_buffer_errors++;
          return NULL;
        }

        // Mark buffer as in use
        buffer->in_use = 1;
        buffer->ref_count = 1;
        buffer->length = 0;
        buffer->timestamp = xTaskGetTickCount();
        memset(buffer->data, 0, BUFFER_SIZE);

        pool_stats.buffers_allocated++;

        xSemaphoreGive(pool_mutex);
        return buffer;
      }

      ESP_LOGE(TAG, "Failed to take mutex in get_free_buffer");
      return NULL;
    }

    // Return a buffer to the free pool (thread-safe)
    esp_err_t LORATracker::return_buffer_to_pool(rx_buffer_t *buffer)
    {
      if (!this->validate_buffer(buffer))
      {
        return ESP_ERR_INVALID_ARG;
      }

      // Take mutex to safely modify buffer state
      if (xSemaphoreTake(pool_mutex, portMAX_DELAY) != pdTRUE)
      {
        ESP_LOGE(TAG, "Failed to take mutex in return_buffer_to_pool");
        return ESP_FAIL;
      }

      // Check for double-free
      if (!buffer->in_use)
      {
        ESP_LOGE(TAG, "Double-free detected for buffer %p", buffer);
        pool_stats.double_free_attempts++;
        xSemaphoreGive(pool_mutex);
        return ESP_ERR_INVALID_STATE;
      }

      // Decrement reference count
      if (buffer->ref_count > 0)
      {
        buffer->ref_count--;
      }

      // Only return to pool if no more references
      if (buffer->ref_count == 0)
      {
        buffer->in_use = 0;
        buffer->length = 0;

        // Clear sensitive data
        memset(buffer->data, 0, BUFFER_SIZE);

        xSemaphoreGive(pool_mutex);

        // Return to free pool
        if (xQueueSend(free_buffer_queue, &buffer, 0) != pdTRUE)
        {
          ESP_LOGE(TAG, "Failed to return buffer to pool (queue full)");
          return ESP_FAIL;
        }

        pool_stats.buffers_freed++;
      }
      else
      {
        xSemaphoreGive(pool_mutex);
      }

      return ESP_OK;
    }

    // Increment reference count (for multiple consumers)
    esp_err_t LORATracker::buffer_ref_inc(rx_buffer_t *buffer)
    {
      if (!this->validate_buffer(buffer))
      {
        return ESP_ERR_INVALID_ARG;
      }

      if (xSemaphoreTake(pool_mutex, portMAX_DELAY) == pdTRUE)
      {
        if (!buffer->in_use)
        {
          ESP_LOGE(TAG, "Attempting to ref unused buffer %p", buffer);
          xSemaphoreGive(pool_mutex);
          return ESP_ERR_INVALID_STATE;
        }

        buffer->ref_count++;
        xSemaphoreGive(pool_mutex);
        return ESP_OK;
      }

      return ESP_FAIL;
    }

    // Validate buffer belongs to pool and has correct magic number
    bool LORATracker::validate_buffer(rx_buffer_t *buffer)
    {
      if (!buffer)
      {
        ESP_LOGE(TAG, "NULL buffer pointer");
        return false;
      }

      // Check if pointer is within pool bounds
      if (buffer < memory_pool || buffer >= &memory_pool[POOL_SIZE])
      {
        ESP_LOGE(TAG, "Buffer pointer %p outside pool range", buffer);
        pool_stats.invalid_buffer_errors++;
        return false;
      }

      // Check magic number
      if (buffer->magic != MAGIC_NUMBER)
      {
        ESP_LOGE(TAG, "Buffer %p has invalid magic: 0x%08lX", buffer, buffer->magic);
        pool_stats.invalid_buffer_errors++;
        return false;
      }

      return true;
    }

  } // namespace esphome::lora_tracker
}
