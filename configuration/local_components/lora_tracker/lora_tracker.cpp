

#include <algorithm>
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
#include <cstring>
// Section 4.4's beacon MAC. PSA rather than a vendored AES: the same library
// the AEAD already runs on, so there is one crypto implementation on this hub.
#include <esp_random.h>
#include <psa/crypto.h>

// The grid geometry. Unconditional: startGrid() and nextT0ForSlotUs() use
// timedgrid:: constants whatever the ESPHome config contains, so an include
// inside #ifdef USE_OTA meant a build without OTA did not compile. Path is the
// component-qualified form every other cross-component include here uses — a
// bare "TimedGrid.h" is not on this component's quoted-include search path
// once ESPHome copies local_components/ into esphome/components/.
#include "esphome/components/lora_client/TimedGrid.h"
// Symbol arithmetic for the Bx receive timestamp (T0 from RxDone). Same
// reasoning as above: qualified path, and unconditional.
#include "esphome/components/lora_client/LoraTiming.h"
// Section 4.4's pending-data bitmap. The tracker publishes it because the
// fleet-wide view is here, where the queues are.
#include "esphome/components/lora_client/PendingData.h"

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
      // Bx: say out loud how well the hub can place an uplink in time. The
      // number is measured, and it is the reason C2 is not buildable on this
      // hardware yet — see the note on last_rx_done_us() in the header.
      ESP_LOGCONFIG(TAG, "  RX timestamp source: main-loop poll (no DIO0 wired)");
      ESP_LOGCONFIG(TAG, "  worst poll gap so far: %u us", this->worst_poll_gap_us_);
    }

    void LORATracker::setup()
    {
      global_lora_tracker = this;

      ESP_LOGI(TAG, "LORATracker setup started");
      this->startGrid();
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
      // Section 4.4's broadcast beacon. Cheap and idempotent — it queues at most
      // one frame per beacon round — so it belongs on the loop rather than on a
      // timer of its own.
      this->serviceBeacon();
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
      // Bx: stamp the poll BEFORE touching the radio.
      //
      // lora_receive_packet() reads REG_IRQ_FLAGS as its first act, so this is
      // the closest the poll path gets to "when we learned a packet was
      // there". RxDone itself happened somewhere in (previous poll, now], so
      // the estimate is the midpoint of that window and the uncertainty is
      // half its width. Measuring the gap rather than assuming loop()'s 10 ms
      // matters: the gap is the delay plus whatever else the main loop did.
      const int64_t poll_us = esp_timer_get_time();
      const int64_t gap_us  = this->have_poll_baseline_ ? (poll_us - this->last_poll_us_) : 0;
      this->last_poll_us_        = poll_us;
      this->have_poll_baseline_  = true;
      if (gap_us > 0 && (uint32_t) gap_us > this->worst_poll_gap_us_)
        this->worst_poll_gap_us_ = (uint32_t) gap_us;

      if (this->radio_mutex_ != nullptr)
        xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);

      int rxLen = lora_receive_packet(buf_, sizeof(buf_));
      if (rxLen > 0)
      {
        // F-11: cache link quality for this packet before dispatch so clients
        // can publish it to Home Assistant.
        this->last_packet_rssi_ = lora_packetRssi();
        this->last_packet_snr_  = lora_packetSnr();

        this->last_rx_done_us_       = poll_us - gap_us / 2;
        this->last_rx_uncertainty_us_ = (uint32_t) (gap_us / 2);
        // T0 = SFD end, the single timing reference used at both ends. Never
        // decomposed by hand — LoraTiming.h owns the symbol arithmetic, and
        // PL is the length the radio reports, which excludes the CRC.
        this->last_rx_t0_us_ =
            loratiming::t0FromRxDoneUs(this->last_rx_done_us_, (uint32_t) rxLen);
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

    // B1a: drain the handoff queue into the reordering queue.
    //
    // data_queue is a FreeRTOS queue because producers run on other tasks and
    // need something they can write from anywhere. It is a FIFO, and a FIFO
    // cannot express "not before round n+2, and behind nothing else" — which
    // is what section 4.5's deferral requires and what B-1 left out on purpose
    // until there was a scheduler to honour it. So the queue is only a handoff:
    // ordering and eligibility are decided here.
    void LORATracker::drainHandoffQueue_()
    {
      rx_buffer_t *rx_buffer = nullptr;
      while (xQueueReceive(data_queue, &rx_buffer, 0) == pdTRUE)
      {
        // Validate before scheduling. Use continue, NOT return: this runs on
        // the persistent TX task, and returning early would leave the rest of
        // the handoff queue unread.
        if (!this->validate_buffer(rx_buffer))
        {
          ESP_LOGE(TAG, "Received invalid buffer, skipping");
          continue;
        }
        if (rx_buffer->length > BUFFER_SIZE)
        {
          ESP_LOGE(TAG, "Buffer length %d exceeds max %d",
                   rx_buffer->length, BUFFER_SIZE);
          this->return_buffer_to_pool(rx_buffer);
          continue;
        }

        const uint8_t slot = (uint8_t) (rx_buffer - memory_pool);
        if (slot >= POOL_SIZE)
        {
          // Not from the pool. Cannot be scheduled by index and cannot be
          // safely returned either; say so rather than corrupting the queue.
          ESP_LOGE(TAG, "Buffer %p is not from the pool, dropping", rx_buffer);
          continue;
        }

        const txqueue::Priority prio =
            (rx_buffer->tx_priority == 0) ? txqueue::Priority::Immediate
          : (rx_buffer->tx_priority >= 2) ? txqueue::Priority::Background
                                          : txqueue::Priority::Normal;

        if (!this->tx_queue_.push(slot, rx_buffer->tx_earliest_us, prio))
        {
          // The scheduler is full. Refuse loudly and return the buffer — the
          // alternative, silently sending it out of order, would defeat the
          // deferral the queue exists to provide.
          ESP_LOGW(TAG, "TX scheduler full (%u entries), dropping %d bytes",
                   (unsigned) this->tx_queue_.size(), (int) rx_buffer->length);
          this->return_buffer_to_pool(rx_buffer);
        }
      }
    }

    int64_t LORATracker::nextTxEligibleUs(int64_t now_us) const
    {
      return this->tx_queue_.nextEligibleUs(now_us);
    }

    bool LORATracker::serviceTxQueue(int64_t now_us)
    {
      this->drainHandoffQueue_();

      // Released up to kPrepareLeadUs EARLY, and with the instant it was
      // scheduled for. A queue that hands a placed frame over only once its
      // instant has passed can never fire on it: the caller inherits the whole
      // prepare cost and B5's prepare/fire split has nothing left to hold back.
      // firePacket()'s not_before_us was written for exactly this and had one
      // call site, hardcoded to 0 — the split was built and never used.
      int64_t fire_at_us = 0;
      const uint8_t slot =
          this->tx_queue_.popDue(now_us, kPrepareLeadUs, fire_at_us);
      if (slot == txqueue::kInvalidSlot)
        return false;

      rx_buffer_t *rx_buffer = &memory_pool[slot];

      ESP_LOGI(TAG, "Processing %d bytes from buffer %p",
               rx_buffer->length, rx_buffer);

      // Declare the channel busy BEFORE transmitting, not after: a caller
      // deciding where to place a timed downlink must see the burst that is
      // about to run, not the one that just finished.
      const int copies = (rx_buffer->tx_copies > 0) ? rx_buffer->tx_copies
                                                    : this->txSlotsPerRound;
      const uint32_t stride_ms = (rx_buffer->tx_stride_ms > 0)
                                 ? rx_buffer->tx_stride_ms
                                 : (uint32_t) this->txIntervalMs;
      // From the instant the frame will actually LEAVE, not from now: with a
      // placed frame those differ by up to kPrepareLeadUs, and this number is
      // what another caller consults to decide whether the channel is free.
      const int64_t tx_start_us = (fire_at_us > now_us) ? fire_at_us : now_us;
      this->burst_busy_until_us_ =
          tx_start_us + (int64_t) (copies - 1) * (int64_t) stride_ms * 1000
                 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) rx_buffer->length)
                 + (int64_t) loratiming::kPreambleToT0Us
                 + (int64_t) this->responseWindowMs * 1000;

      lora_tx_busy_ = true;
      this->sendPacketBurst(rx_buffer->data, rx_buffer->length,
                            rx_buffer->tx_copies, rx_buffer->tx_stride_ms,
                            fire_at_us);
      lora_tx_busy_ = false;

      if (rx_buffer->length > 0)
      {
        ESP_LOG_BUFFER_HEX(TAG, rx_buffer->data, rx_buffer->length);
      }

      this->return_buffer_to_pool(rx_buffer);
      return true;
    }

    void LORATracker::sendTask(void *pvParameters)
    {
      (void)pvParameters;

      for (;;)
      {
        const int64_t now_us = esp_timer_get_time();

        if (this->serviceTxQueue(now_us))
        {
          // Post-burst response window.  The addressed node defers its reply
          // (ACK / position) until just after this burst ends, then transmits
          // into the clear channel.  Hold off dequeuing the next burst and keep
          // the radio in RX so that reply is heard instead of being stepped on
          // by the next burst.  lora_tx_busy_ is already false, so the main
          // loop's receive() reads any incoming packet during this window.
          //
          // This window is also why deferral is TWO rounds and not one: burst
          // plus window is ~1850 ms against a 1500 ms round, so a frame held
          // to round n+1 would land inside this very wait. See
          // txqueue::kDeferRounds.
          if (this->radio_mutex_ != nullptr)
            xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);
          lora_receive(0);
          if (this->radio_mutex_ != nullptr)
            xSemaphoreGive(this->radio_mutex_);
          vTaskDelay(pdMS_TO_TICKS(this->responseWindowMs));
          continue;
        }

        // Nothing eligible. Sleep until either something becomes eligible or a
        // producer hands over a new frame, whichever comes first.
        //
        // This replaces an xQueueReceive(portMAX_DELAY): with a deferred frame
        // in the scheduler, blocking forever on the handoff queue would hold
        // that frame until some UNRELATED traffic happened to arrive and wake
        // the task. The peek is what makes the wait interruptible — the frame
        // it sees is drained at the top of the next pass, not here.
        const int64_t next_us = this->tx_queue_.nextEligibleUs(now_us);
        TickType_t wait_ticks;
        if (next_us == INT64_MAX)
        {
          wait_ticks = portMAX_DELAY;   // genuinely idle
        }
        else
        {
          // Wake kPrepareLeadUs early, or the frame is popped only after its
          // instant has passed and there is nothing left to place it with.
          const int64_t delta_us = next_us - now_us - kPrepareLeadUs;
          wait_ticks = (delta_us <= 0) ? 0
                                       : pdMS_TO_TICKS(delta_us / 1000 + 1);
        }

        rx_buffer_t *peeked = nullptr;
        xQueuePeek(data_queue, &peeked, wait_ticks);
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

    // -----------------------------------------------------------------------
    // B1: the grid anchor.
    // -----------------------------------------------------------------------
    void LORATracker::startGrid()
    {
      if (this->grid_started_)
        return;   // set once, never moved
      this->grid_anchor_us_ = esp_timer_get_time();
      this->grid_started_   = true;

      // PSA is initialised HERE, not left to whichever component happened to
      // set up first. LORAListener::setup() calls psa_crypto_init() for the
      // AEAD, and the tracker's beacon MAC used to be able to free-ride on
      // that — but component setup order is not a contract, and the failure it
      // produces is psa_import_key: -137 (BAD_STATE) at beacon time, which
      // reads as a crypto fault rather than as an ordering one. The call is
      // idempotent, which is what makes stating the dependency free.
      const psa_status_t psa_ret = psa_crypto_init();
      if (psa_ret != PSA_SUCCESS)
        ESP_LOGE(TAG, "psa_crypto_init failed: %d — beacons cannot be signed",
                 (int) psa_ret);

      // Section 4.4's fleet key, minted WITH the anchor.
      //
      // Not derived from a compile-time secret, which was the obvious cheaper
      // option and is worth naming as rejected: a key every node can compute is
      // a key every node can forge under, and the whole point of authenticating
      // the beacon is that only the hub may move an anchor or clear a bit.
      //
      // Re-minted on every restart, and that costs nothing — a restart already
      // invalidates every node's anchor and forces GridSync to be re-published,
      // so the key's lifetime is exactly the grid's and there is nothing to
      // persist. The id is random for the same reason: a counter would need NVS
      // on a hub that has just lost the state a counter exists to survive.
      for (size_t i = 0; i < sizeof(this->net_key_); i += 4)
      {
        const uint32_t r = esp_random();
        std::memcpy(this->net_key_ + i, &r,
                    std::min(sizeof(uint32_t), sizeof(this->net_key_) - i));
      }
      // Zero means "no key" to every reader of this field, so a one-in-four-
      // billion draw must not be allowed to mean it.
      do { this->net_key_id_ = esp_random(); } while (this->net_key_id_ == 0);
      ESP_LOGI(TAG, "Fleet key minted for the grid (id %08x)",
               (unsigned) this->net_key_id_);
      ESP_LOGI(TAG, "Grid anchor set at %lld us (%u slots of %u us in %u us)",
               (long long) this->grid_anchor_us_,
               (unsigned) timedgrid::kSlotCount,
               (unsigned) timedgrid::kSlotPitchUs,
               (unsigned) timedgrid::kRoundUs);
    }

    uint32_t LORATracker::roundForSlotT0(uint8_t slot, int64_t t0_us) const
    {
      if (!this->grid_started_)
        return 0;
      const int64_t rel = t0_us - this->grid_anchor_us_
                        - (int64_t) (slot % timedgrid::kSlotCount)
                            * (int64_t) timedgrid::kSlotPitchUs;
      if (rel < 0)
        return 0;
      return (uint32_t) (rel / (int64_t) timedgrid::kRoundUs);
    }

    uint32_t LORATracker::beaconRoundForT0(int64_t t0_us) const
    {
      return this->roundForSlotT0((uint8_t) timedgrid::kBeaconSlotIndex, t0_us);
    }

    int64_t LORATracker::nextBeaconT0Us(int64_t now_us) const
    {
      if (!this->grid_started_ || timedgrid::kBeaconEveryRounds == 0)
        return 0;
      // The beacon slot's marks come every round; only every Nth is a beacon.
      const int64_t stride = (int64_t) timedgrid::kRoundUs
                           * (int64_t) timedgrid::kBeaconEveryRounds;
      const int64_t base   = this->grid_anchor_us_
                           + (int64_t) timedgrid::kBeaconSlotIndex
                               * (int64_t) timedgrid::kSlotPitchUs;
      if (now_us <= base)
        return base;
      return base + ((now_us - base + stride - 1) / stride) * stride;
    }

    // The beacon's authenticator: AES-CMAC over the fields the beacon carries.
    //
    // A MAC, not the AEAD the rest of the link uses, and the reasoning is in
    // FrameCrypto.h: nothing in a beacon is secret, and extending AES-GCM to a
    // one-to-many key would need a never-repeating counter that a hub reboot
    // restarts. CMAC has no nonce to reuse.
    //
    // The key is imported per call rather than held in a PSA slot. A beacon is
    // one frame every 350 s, so the import costs nothing measurable, and a slot
    // held across a startGrid() would authenticate the NEW grid's beacons with
    // the OLD grid's key — silently, because both ends would simply stop
    // agreeing.
    bool LORATracker::beaconMac(uint32_t tx_round, uint32_t tx_slot,
                                uint32_t pending_mask, bool pending_mask_valid,
                                uint8_t *out, size_t out_len) const
    {
      if (out == nullptr || out_len != framecrypto::kBeaconMacBytes)
        return false;
      if (!framecrypto::netKeyIsSet(this->net_key_, sizeof(this->net_key_),
                                    this->net_key_id_))
        return false;

      uint8_t input[framecrypto::kBeaconMacInputBytes];
      framecrypto::buildBeaconMacInput(this->net_key_id_, tx_round, tx_slot,
                                       pending_mask, pending_mask_valid, input);

      psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
      psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
      psa_set_key_algorithm(&attrs, PSA_ALG_CMAC);
      psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
      psa_set_key_bits(&attrs, framecrypto::kNetKeyBytes * 8);
      psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

      psa_key_id_t kid = PSA_KEY_ID_NULL;
      psa_status_t st = psa_import_key(&attrs, this->net_key_,
                                       sizeof(this->net_key_), &kid);
      if (st != PSA_SUCCESS)
      {
        ESP_LOGE(TAG, "beacon MAC: psa_import_key failed: %d", (int) st);
        return false;
      }

      // The full CMAC is 16 bytes and only the first 8 go on the air, so it is
      // computed into a full-width buffer and truncated here. Asking PSA for a
      // short output would fail rather than truncate.
      uint8_t full[16];
      size_t  full_len = 0;
      st = psa_mac_compute(kid, PSA_ALG_CMAC, input, sizeof(input),
                           full, sizeof(full), &full_len);
      psa_destroy_key(kid);
      if (st != PSA_SUCCESS || full_len < framecrypto::kBeaconMacBytes)
      {
        ESP_LOGE(TAG, "beacon MAC: psa_mac_compute failed: %d", (int) st);
        return false;
      }
      std::memcpy(out, full, framecrypto::kBeaconMacBytes);
      return true;
    }

    // Section 4.4. The frame that keeps a node IN Mode B.
    //
    // Without it a node holds phase only for resyncMaxS after each addressed
    // frame — at 3.5 commands/day that is 2.9 % of the day on the grid, and no
    // measurable battery saving, so the mode would buy a narrower window for
    // nothing. Cost to the hub is FLAT IN NODE COUNT: one broadcast, 13.1 s of
    // air per day at the default cadence, against 80.1 s/day for today's
    // bursts.
    void LORATracker::serviceBeacon()
    {
      if (!this->grid_started_ || timedgrid::kBeaconEveryRounds == 0)
        return;

      const int64_t now = esp_timer_get_time();
      const int64_t t0  = this->nextBeaconT0Us(now);
      if (t0 <= 0)
        return;

      const uint32_t round = this->beaconRoundForT0(t0);
      if (round == this->beacon_round_queued_)
        return;                       // already placed for this beacon

      // Queue only once the mark is close enough that a placed frame is worth
      // holding a buffer for, and no closer than the queue can fire on. The
      // buffer pool is five deep, so a beacon queued minutes ahead would hold a
      // fifth of it for the whole interval.
      const int64_t fire = loratiming::fireInstantUs(t0, 0);
      const int64_t lead = fire - now;
      if (lead > (int64_t) timedgrid::kRoundUs || lead < txqueue::kPrepareLeadUs)
        return;

      LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress   = LORATracker::broadcastAddressing;
      header.destsubnet    = LORATracker::subnetAddressing;
      header.senderaddress = esphome::lora_tracker::kHubAddress;
      // A broadcast belongs to no per-node sequence, and the nodes exempt it
      // from their replay filter for exactly that reason: running it through
      // one would ratchet every node's rx counter onto the beacon's. Its replay
      // protection is txRound — a replayed beacon predicts a mark rounds in the
      // past, which no node will accept.
      header.msgid         = 0;
      header.burstindex    = 0;
      header.burstcount    = 1;
      op.header = &header;

      GridBeacon gb = GRID_BEACON__INIT;
      gb.txround = round;
      gb.txslot  = timedgrid::kBeaconSlotIndex;
      // ALL LISTENING, deliberately, and still not a placeholder.
      //
      // A clear bit tells a node to stop listening for up to a beacon interval.
      // Two independent things had to be true before this hub could clear one.
      // ONE OF THEM IS NOW TRUE and the other is not:
      //
      //   * The beacon was unauthenticated, so the nodes refused a mask from it
      //     outright. That is fixed below: it carries an AES-CMAC under the
      //     fleet key, and a node holding that key adopts the mask.
      //   * A cleared bit is still a promise this hub cannot keep for an
      //     INTERACTIVE node: Home Assistant can produce a command at any
      //     instant, and the node would not be listening for up to 5.8 minutes.
      //     Tier 3 suits automatic-mode nodes, which are already unreachable
      //     between check-ins by design (section 5.5).
      //
      // So authenticating the beacon does NOT by itself start clearing bits.
      // What it buys today is the other thing an unauthenticated beacon could
      // do: nudge the anchor. The guard band bounded ONE nudge, not a sequence
      // of them, so anything in radio range could walk a node off the grid
      // 14 ms at a time. Only the hub can now.
      gb.pendingmask      = pending::allListening();
      gb.pendingmaskvalid = true;

      // Signed over exactly the fields above, plus the key id, so a node can
      // tell "this hub" from "something in radio range". No timestamp: a
      // replayed beacon declares its own round, so its predicted mark is rounds
      // in the past and the node's reanchorIsSane already refuses it.
      uint8_t mac[framecrypto::kBeaconMacBytes];
      if (!this->beaconMac(gb.txround, gb.txslot, gb.pendingmask,
                           gb.pendingmaskvalid, mac, sizeof(mac)))
      {
        // No beacon rather than an unsigned one. A hub that HAS a key and emits
        // an unsigned beacon is indistinguishable on the air from an attacker,
        // and every node holding the key would refuse it anyway — so sending it
        // costs air time and buys nothing. Not recorded as queued: a hub that
        // has stopped being able to sign should show as a hub that has stopped
        // beaconing.
        ESP_LOGE(TAG, "beacon for round %u could not be signed — not sent",
                 (unsigned) round);
        return;
      }
      gb.netkeyid  = this->net_key_id_;
      gb.mac.data  = mac;
      gb.mac.len   = sizeof(mac);

      op.cmd_case   = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON;
      op.gridbeacon = &gb;

      const size_t len = lora_client_operation_message__get_packed_size(&op);
      if (len == 0 || len > BUFFER_SIZE)
      {
        ESP_LOGE(TAG, "beacon would not fit (%u B)", (unsigned) len);
        this->beacon_round_queued_ = round;   // do not retry it every loop
        return;
      }
      std::vector<uint8_t> buf(len);
      lora_client_operation_message__pack(&op, buf.data());

      TxPolicy p;
      p.copies      = 1;              // one broadcast, on one mark
      p.stride_ms   = 0;
      p.earliest_us = fire;
      p.priority    = 0;              // a beacon that slips its mark is useless
      if (!this->send(buf.data(), buf.size(), p))
      {
        // Dropped rather than queued: no pool buffer. Say so, and let the next
        // beacon round try again — recording the round as queued would hide a
        // hub that has stopped beaconing entirely.
        ESP_LOGW(TAG, "beacon for round %u was dropped, not queued",
                 (unsigned) round);
        return;
      }

      this->beacon_round_queued_ = round;
      this->beacons_sent_++;
      ESP_LOGI(TAG, "beacon queued: round %u, slot %u, T0 %lld us, fire %lld us",
               (unsigned) round, (unsigned) timedgrid::kBeaconSlotIndex,
               (long long) t0, (long long) fire);
    }

    int64_t LORATracker::nextT0ForSlotUs(uint8_t slot, int64_t now_us) const
    {
      if (!this->grid_started_)
        return now_us;   // no grid: send now, do not invent an instant

      const int64_t pitch = (int64_t) timedgrid::kSlotPitchUs;
      const int64_t round = (int64_t) timedgrid::kRoundUs;
      const int64_t base  = this->grid_anchor_us_
                          + (int64_t) (slot % timedgrid::kSlotCount) * pitch;

      // Rounds elapsed since that slot's first T0, rounded UP so the answer is
      // never in the past. Integer division truncates towards zero, which for a
      // negative delta would round the wrong way — hence the explicit branch
      // rather than a (d + round - 1) / round that only works for d >= 0.
      const int64_t delta = now_us - base;
      if (delta <= 0)
        return base;
      const int64_t rounds = (delta + round - 1) / round;
      return base + rounds * round;
    }

    uint32_t LORATracker::msUntilNextT0(uint8_t slot) const
    {
      if (!this->grid_started_)
        return 0;
      const int64_t now = esp_timer_get_time();
      const int64_t t0  = this->nextT0ForSlotUs(slot, now);
      const int64_t d   = t0 - now;
      return d <= 0 ? 0u : (uint32_t) ((d + 999) / 1000);
    }

    // When the channel is next free of the hub's own burst.
    //
    // A 17-copy burst denies 31 of the 32 slots in the round it runs in — the
    // 88 ms stride is 1.878 slot pitches, so copies walk ACROSS slot boundaries
    // rather than landing on them, and each copy's 42 ms shadow clips the slots
    // on both sides (measured; see section 4.5). There is no hole to interleave
    // a timed downlink into, so the only correct answer for a Mode B node whose
    // slot falls in that round is to move its frame out of the round entirely.
    //
    // The window extends past the last copy by responseWindowMs, because the
    // addressed node defers ITS reply into that gap and sendTask holds the
    // radio there. Burst plus window is ~1850 ms against a 1500 ms round, which
    // is why the deferral is two rounds and not one.
    int64_t LORATracker::busyUntilUs() const
    {
      return this->burst_busy_until_us_;
    }

    // The next T0 for `slot` that is not inside a burst.
    //
    // Not simply "the next T0 after busyUntil": a caller wants the node's own
    // mark, and the node is only listening at its own marks. Skipping to the
    // first T0 at or after the channel clears is exactly that — the grid
    // arithmetic already spaces those a round apart.
    int64_t LORATracker::nextClearT0ForSlotUs(uint8_t slot, int64_t now_us) const
    {
      const int64_t floor_us = (this->burst_busy_until_us_ > now_us)
                             ? this->burst_busy_until_us_ : now_us;
      return this->nextT0ForSlotUs(slot, floor_us);
    }

    uint32_t LORATracker::msUntilNextClearT0(uint8_t slot) const
    {
      if (!this->grid_started_)
        return 0;
      const int64_t now = esp_timer_get_time();
      const int64_t t0  = this->nextClearT0ForSlotUs(slot, now);
      const int64_t d   = t0 - now;
      return d <= 0 ? 0u : (uint32_t) ((d + 999) / 1000);
    }

    bool LORATracker::send(uint8_t *data, size_t len, const TxPolicy &policy)
    {
      // Validate inputs first
      if (!data || len == 0)
      {
        ESP_LOGW(TAG, "Invalid send parameters: data=%p, len=%zu", data, len);
        return false;
      }

      // Check queue initialization
      if (free_buffer_queue == nullptr || data_queue == nullptr)
      {
        ESP_LOGE(TAG, "Queues not initialized. free_buffer_queue=%p, data_queue=%p",
                 free_buffer_queue, data_queue);
        return false;
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

        // The policy travels WITH the frame. Anything stored on the tracker
        // instead would be read by sendTask at dequeue, by which time the
        // caller that set it may be long gone and a different frame may be at
        // the head of the queue.
        rx_buffer->tx_copies      = policy.copies;
        rx_buffer->tx_stride_ms   = policy.stride_ms;
        rx_buffer->tx_earliest_us = policy.earliest_us;
        rx_buffer->tx_priority    = policy.priority;

        // Send buffer pointer to data queue
        if (xQueueSend(data_queue, &rx_buffer, 0) != pdTRUE)
        {
          ESP_LOGW(TAG, "Data queue full, dropping data");
          this->return_buffer_to_pool(rx_buffer);
          return false;
        }
        return true;
      }

      ESP_LOGW(TAG, "No free buffers, dropped %d bytes", len);
      return false;
    }

    void LORATracker::sendPacketBurst(uint8_t *data, size_t len, int copies,
                                      uint32_t stride_ms, int64_t not_before_us)
    {

      // const TickType_t xFrequency = pdMS_TO_TICKS(142); // For RX /TX config 3x RX + 7x TX
      // const TickType_t xFrequency = pdMS_TO_TICKS(59); // For RX /TX config 3x RX + 17x TX in 1sec
      // Both come from the FRAME, not from tracker state; 0 means "the default".
      const TickType_t xFrequency =
          pdMS_TO_TICKS(stride_ms > 0 ? stride_ms : (uint32_t) this->txIntervalMs);
      const int burstCopies = (copies > 0) ? copies : this->txSlotsPerRound;

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
        burstMsg->header->burstcount = burstCopies;

      // for (int cnt = 0; cnt < 7; cnt++)
      for (int cnt = 0; cnt < burstCopies; cnt++)
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
            // Only copy 0 is placed. The rest are paced by vTaskDelayUntil at
            // the burst stride, which is the construction they have always had
            // — and a placed frame is a SINGLE copy anyway (C2's RX1 reply),
            // so for it this loop runs once.
            this->sendPacketAt(cbuf, clen, (cnt == 0) ? not_before_us : 0);
            free(cbuf);
          }
          else
          {
            // OOM fallback: unindexed copy, still placed.
            this->sendPacketAt(data, len, (cnt == 0) ? not_before_us : 0);
          }
        }
        else
        {
          this->sendPacketAt(data, len, (cnt == 0) ? not_before_us : 0);
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
        if (cnt + 1 < burstCopies)
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

    // ---- B5: PREPARE / FIRE -------------------------------------------
    //
    // Everything a transmit does splits cleanly in two. PREPARE is idling the
    // radio, setting the TX preamble, arming the FIFO pointer and clocking the
    // payload in — SPI work whose duration varies with payload length and with
    // whatever else is on the bus. FIRE is one register write, and it is the
    // instant the frame actually starts going out.
    //
    // Doing them together means the fire instant inherits all of PREPARE's
    // variance, which is what B5's "p99 fire residual < 200 us" gate is about
    // and what Mode B's grid needs bounded. Separated, a caller can prepare
    // early — at leisure, whenever the radio is free — and fire at a computed
    // instant with only one register write in front of it.
    //
    // The radio mutex is taken by PREPARE and released by FIRE. That is
    // deliberate: between them the FIFO holds a half-built frame, and a
    // concurrent receive on the main loop would read it out from under us.
    // tx_prepared_ makes a missing FIRE detectable rather than a deadlock.
    // ---------------------------------------------------------------------

    bool LORATracker::preparePacket(uint8_t *data, size_t len)
    {
      if (this->tx_prepared_)
      {
        // A previous PREPARE was never fired. Its mutex is still held and its
        // bytes are still in the FIFO; overwriting them silently would send a
        // spliced frame. Abandon the old one explicitly.
        ESP_LOGE(TAG, "preparePacket called twice, discarding the first");
        this->abortPreparedPacket();
      }

      if (this->radio_mutex_ != nullptr)
        xSemaphoreTake(this->radio_mutex_, portMAX_DELAY);

      lora_idle();

      // Only the preamble length is per-packet.
      //
      // Spreading factor, coding rate, bandwidth, sync word and CRC were being
      // re-written before EVERY copy of every burst, each one a register
      // read-modify-write over SPI, none of them ever changing: setup() above
      // already writes exactly these values and nothing alters them at
      // runtime. Seventeen copies paid for it seventeen times, all of it
      // between the caller's decision to send and the radio actually firing.
      //
      // The preamble genuinely does alternate (TX uses loraPreambleLengthTx,
      // RX loraPreambleLengthRx, restored after the packet in firePacket), so
      // it stays.
      lora_setPreambleLength(loraPreambleLengthTx);

      const int status = lora_beginPacket();
      if (status == 0)
      {
        ESP_LOGW(TAG, "Already Xmitting");
      }

      lora_write(data, len);
      this->tx_prepared_ = true;
      return status != 0;
    }

    void LORATracker::abortPreparedPacket()
    {
      if (!this->tx_prepared_)
        return;

      // Leave the radio listening rather than half-armed for a transmit.
      lora_setPreambleLength(loraPreambleLengthRx);
      lora_receive(0);
      this->tx_prepared_ = false;

      if (this->radio_mutex_ != nullptr)
        xSemaphoreGive(this->radio_mutex_);
    }

    bool LORATracker::firePacket(int64_t not_before_us)
    {
      if (!this->tx_prepared_)
      {
        ESP_LOGE(TAG, "firePacket with nothing prepared");
        return false;
      }

      // Hold to the requested instant. A busy wait, deliberately: the whole
      // point of the split is that nothing schedulable stands between here and
      // the register write, and at these distances (a caller that prepared
      // early is waiting hundreds of microseconds, not milliseconds) yielding
      // would reintroduce exactly the variance being removed. A caller that
      // wants to wait longer should prepare later.
      if (not_before_us > 0)
      {
        // The cap has to bound the DEADLINE, not the first sleep. Capping
        // `remaining` and then recomputing it from not_before_us inside the
        // loop put the ceiling back where it started on every iteration: the
        // loop spun to not_before_us however far away that was. Since
        // preparePacket() takes the radio mutex and only firePacket() gives it,
        // an uncapped spin here holds the mutex — and so blocks the main
        // ESPHome loop, which takes it in checkReception() — until the task
        // watchdog fires, with nothing logged to say why.
        const int64_t deadline =
            std::min(not_before_us,
                     esp_timer_get_time() + (int64_t) kMaxFireBusyWaitUs);
        if (deadline < not_before_us)
        {
          ESP_LOGW(TAG, "firePacket asked to wait %lld us, capping at %d",
                   (long long) (not_before_us - esp_timer_get_time()),
                   (int) kMaxFireBusyWaitUs);
        }
        int64_t remaining = deadline - esp_timer_get_time();
        while (remaining > 0)
        {
          esphome::delayMicroseconds((uint32_t) remaining);
          remaining = deadline - esp_timer_get_time();
        }
      }

      lora_tx();   // THE fire instant

      const int ok = lora_waitTxDone();
      if (ok == 0)
      {
        ESP_LOGW(TAG, "TX timeout");
      }

      lora_setPreambleLength(loraPreambleLengthRx);
      this->tx_prepared_ = false;

      if (this->radio_mutex_ != nullptr)
        xSemaphoreGive(this->radio_mutex_);

      // Outside the mutex: ESP_LOGI formats and writes to the UART, and at
      // 115200 baud a 40-character line is ~3.5 ms of the radio held idle.
      ESP_LOGI(TAG, "Packet sent");
      return ok != 0;
    }

    void LORATracker::sendPacketBytes(uint8_t *data, size_t len)
    {
      this->sendPacketAt(data, len, /*not_before_us=*/0);
    }

    void LORATracker::sendPacketAt(uint8_t *data, size_t len, int64_t not_before_us)
    {
      // Logging BEFORE the mutex, for the same reason as in firePacket.
      ESP_LOGI(TAG, "Sending packet of length %d", len);

      this->preparePacket(data, len);
      this->firePacket(not_before_us);
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
