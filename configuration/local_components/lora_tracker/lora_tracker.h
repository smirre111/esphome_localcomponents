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
// B1a: the reordering transmit queue. Dependency-free (stdint only), so
// including it here costs nothing; qualified path for the same reason as
// TimedGrid.h in the .cpp.
#include "esphome/components/lora_client/TxQueue.h"

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
  // How THIS frame is to be transmitted. Travels with the buffer because the
  // policy is decided at enqueue and consumed at dequeue, and those are
  // different moments on different tasks — see TxPolicy.
  int      tx_copies;     // 0 = the default full burst
  uint32_t tx_stride_ms;  // 0 = the default txIntervalMs
  int64_t  tx_earliest_us; // 0 = no constraint
  uint8_t  tx_priority;    // txqueue::Priority as an integer
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
  // How one frame is transmitted. At NAMESPACE scope, not nested in
  // LORATracker: lora_client.h only forward-declares the tracker, and a nested
  // type of an incomplete class cannot be named. It describes a frame anyway.
  struct TxPolicy
  {
    int      copies{0};     // 0 = txSlotsPerRound, the normal burst
    uint32_t stride_ms{0};  // 0 = txIntervalMs

    // B1a. Not before this instant, on the hub's esp_timer clock; 0 means
    // "as soon as the radio is free". This is what section 4.5's deferral
    // needs and what B-1 deliberately left out until there was a scheduler
    // that could honour it — a FIFO cannot express "later than the frame
    // behind me".
    //
    // THIS IS A FIRE INSTANT, NOT A T0. It is the moment lora_tx() is called,
    // i.e. when the first chirp leaves the antenna. The receiver's reference
    // T0 is kPreambleToT0Us (3136 us) LATER, plus the unmeasured PA ramp.
    //
    // A producer that knows the T0 it wants must convert:
    //     policy.earliest_us = loratiming::fireInstantUs(t0, d_tx_ramp_us);
    // Passing a wanted T0 here directly puts the frame 3136 us late at the
    // receiver, which spends 22 % of a 14080 us guard band before the link
    // has done anything at all.
    int64_t  earliest_us{0};
    // Lower numbers go first; matches txqueue::Priority, kept as a plain
    // integer so this header does not force TxQueue.h on every includer.
    // 0 = Immediate, 1 = Normal, 2 = Background.
    uint8_t  priority{1};
  };



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
      // How one frame is transmitted.
      //
      // This replaces setBurstCopies(), which was a MUTABLE FIELD ON THE
      // TRACKER read at burst time. The copy count was chosen by the caller at
      // enqueue and read by sendTask at dequeue, so every frame queued while
      // the field was set inherited it: during a 300 s drift test, a user
      // pressing a blind button in Home Assistant had their command sent as ONE
      // copy instead of seventeen — about 5.8 % delivery against a node in the
      // ordinary 3-window mode. The old code's own comment worried about
      // leaving the field set afterwards, which is the smaller half of the
      // problem.
      //
      // There is deliberately no first_mark_us here yet. Placing a frame at an
      // absolute instant needs the reordering transmit scheduler (B1a); a field
      // nothing honours would be worse than its absence.

      // --- B1: the grid anchor (implementation-plan.md 4.2) ---------------
      //
      //   T0_k(n) = A + n * kRoundUs + k * kSlotPitchUs
      //
      // A is set ONCE, here, and never moved. That is what lets a node hold a
      // phase across hours; it is also why a hub restart invalidates every
      // node's phase at once, and why the grid must be withdrawn on startup
      // before anything else is transmitted.
      //
      // The anchor lives on the tracker rather than per client because there
      // is one grid per radio: two anchors would put two nodes on overlapping
      // slots while each believed it owned its own.
      void      startGrid();
      int64_t   gridAnchorUs() const { return this->grid_anchor_us_; }
      bool      gridStarted() const  { return this->grid_started_; }

      // The next T0 for `slot` at or after `now_us`. Returns now_us itself when
      // the grid has not started, so a caller that ignores gridStarted() sends
      // immediately rather than at an arbitrary instant derived from a zero
      // anchor.
      int64_t   nextT0ForSlotUs(uint8_t slot, int64_t now_us) const;

      // Milliseconds to wait before a frame for `slot` would start on the
      // grid. 0 means "send now" — both when the grid is not running and when
      // T0 is already upon us, so a caller never has to special-case either.
      // Rounds UP: arriving a millisecond early would put the frame in the
      // previous slot's tail.
      uint32_t  msUntilNextT0(uint8_t slot) const;

      // Section 4.5's slot-aware deferral. A burst denies 31 of the 32 slots in
      // the round it runs in, so a timed downlink whose mark falls inside one
      // must move to the next CLEAR mark rather than be transmitted into it.
      int64_t   busyUntilUs() const;
      int64_t   nextClearT0ForSlotUs(uint8_t slot, int64_t now_us) const;
      uint32_t  msUntilNextClearT0(uint8_t slot) const;

      // Returns false when the frame was DROPPED rather than queued — no free
      // pool buffer, or the handoff queue full. Callers that merely want the
      // frame out may ignore it; a caller that consumed a scarce resource to
      // build the frame (a grid mark, a tracked-op slot) must not record that
      // resource as spent when the frame never entered the queue.
      bool send(uint8_t *data, size_t len) { return this->send(data, len, TxPolicy{}); }
      bool send(uint8_t *data, size_t len, const TxPolicy &policy);
      void sendPacketOnce(uint8_t *data, size_t len);
      // How early the scheduler releases a PLACED frame so the caller has time
      // to prepare before firing. It has to cover one FreeRTOS tick of wake
      // jitter (1 ms at the hub's CONFIG_FREERTOS_HZ = 1000) plus the SPI
      // prepare, and it is what firePacket then busy-waits out. Well inside
      // kMaxFireBusyWaitUs, which is the ceiling for a MISCOMPUTED instant, not
      // a budget to spend.
      static constexpr int64_t kPrepareLeadUs = 5000;

      // Ceiling on firePacket's busy wait. A caller asking for more than this
      // has computed its instant wrongly — most likely against the wrong clock
      // — and spinning for it would starve the task and trip the watchdog.
      // One slot pitch is generous: a correctly prepared frame waits
      // microseconds. Public so a test can assert the bound rather than
      // restate the number, which is how two constants drift apart.
      static constexpr int64_t kMaxFireBusyWaitUs = 46875;

      void sendPacketBurst(uint8_t *data, size_t len, int copies = 0,
                           uint32_t stride_ms = 0, int64_t not_before_us = 0);

      // Drift test: emit ONE copy instead of txSlotsPerRound.
      //
      // Deliberately reuses the burst path rather than adding a second
      // transmit route. An earlier attempt called sendPacketOnce straight from
      // an esp_timer callback and wedged the radio: that bypasses the TxDone
      // handling and the return to RX which live in the main loop, so the
      // SX1278 was left in TX and every later beginPacket failed. The hub went
      // silent until it was restarted.
      //
      // 0 restores the normal 17-copy burst.
      void sendPacketBytes(uint8_t *data, size_t len);
      // sendPacketBytes with a fire instant: prepare now, fire at not_before_us.
      // 0 means fire as soon as the payload is in the FIFO, which is what
      // sendPacketBytes does.
      void sendPacketAt(uint8_t *data, size_t len, int64_t not_before_us);

      // B5's prepare/fire split. See the banner above preparePacket in the
      // .cpp for why the radio mutex spans the pair.
      bool preparePacket(uint8_t *data, size_t len);
      bool firePacket(int64_t not_before_us = 0);
      void abortPreparedPacket();
      void sendTask(void *pvParameters);

      // B1a: one pass of the transmit scheduler — drain the handoff queue into
      // the reordering queue, then send the frame that is eligible now, if any.
      // Returns true if a frame was transmitted.
      //
      // Split out of sendTask's for(;;) so it can be tested: the task itself is
      // an infinite loop with blocking waits, and the interesting behaviour —
      // which frame goes next, and when — is all here.
      bool serviceTxQueue(int64_t now_us);
      void drainHandoffQueue_();
      // When the scheduler next has something to send. INT64_MAX if idle.
      int64_t nextTxEligibleUs(int64_t now_us) const;
      void register_listener(LORAListener *listener);
      void register_client(LORAClient *client);

      // F-11: link-quality of the most recently received packet.  Updated in
      // checkReception() before the packet is dispatched, so a client that
      // accepts the packet can read the value that belongs to it.
      int get_last_rssi() const { return this->last_packet_rssi_; }
      float get_last_snr() const { return this->last_packet_snr_; }

      // --- Bx: hub RX timestamping -----------------------------------------
      //
      // Before this the hub had no timestamp of ANY kind — esp_timer_get_time,
      // micros() and millis() appeared zero times in this component — so an
      // uplink's position in time was simply unknown, which is why Class A
      // (C2) could not be built against it.
      //
      // What it can know today is bounded by how it learns about a packet: a
      // poll from loop(), with esphome::delay(10) per iteration. RxDone
      // happened somewhere between the previous poll and this one, so the
      // unbiased estimate is the MIDPOINT of that interval and the uncertainty
      // is half its width. Both are reported rather than assumed; the interval
      // is measured, because 10 ms is the delay, not the period.
      //
      // This does NOT meet C2's ±1 ms gate and cannot: that needs DIO0 wired
      // to the ESP32 and an ISR stamp, exactly as the node does it
      // (g_dio0_rx_us in isr_pinLoraDIO0). DIO0 is not in the hub's pin map at
      // all, so the ISR half is a hardware change, not a code one. The
      // arithmetic above it — T0 from RxDone via LoraTiming.h — is the same
      // either way, so it is written once, here, and an ISR stamp would only
      // replace where last_rx_done_us_ comes from.
      //
      // Clients read these inside set_response(), which the tracker calls
      // synchronously on the same task immediately after updating them — the
      // same contract get_last_rssi() already relies on. That is why the
      // timestamp is not threaded through set_response()'s signature.
      int64_t  last_rx_done_us() const { return this->last_rx_done_us_; }
      int64_t  last_rx_t0_us() const   { return this->last_rx_t0_us_; }
      uint32_t rx_stamp_uncertainty_us() const {
        return this->last_rx_uncertainty_us_;
      }
      // Worst poll gap seen since boot. The uncertainty above is per-packet;
      // this is what a bench procedure should report.
      uint32_t worst_poll_gap_us() const { return this->worst_poll_gap_us_; }

    protected:
      int   last_packet_rssi_{0};
      float last_packet_snr_{0.0f};

      // An explicit flag, not `last_poll_us_ > 0`. esp_timer_get_time() starts
      // near zero at boot, so a sentinel of 0 makes the first polls report a
      // gap of 0 — an uncertainty of ±0 µs, which is a false claim of
      // precision at exactly the moment the hub knows least.
      bool     have_poll_baseline_{false};
      int64_t  last_poll_us_{0};
      int64_t  last_rx_done_us_{0};
      int64_t  last_rx_t0_us_{0};
      uint32_t last_rx_uncertainty_us_{0};
      uint32_t worst_poll_gap_us_{0};

      esp_err_t init_memory_pool(void);
      int64_t grid_anchor_us_{0};
      bool    grid_started_{false};
      // When the hub's own burst stops occupying the channel, including the
      // post-burst response window sendTask holds the radio in.
      int64_t burst_busy_until_us_{0};

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
      // B1a: the reordering, time-scheduled transmit queue. data_queue stays as
      // the handoff from producer tasks — it is a FreeRTOS queue and can be
      // written from anywhere — and sendTask drains it into this, which is
      // where ordering and eligibility are decided.
      txqueue::Queue tx_queue_;

      // A frame is in the FIFO and the radio mutex is held, waiting for FIRE.
      bool tx_prepared_{false};


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
