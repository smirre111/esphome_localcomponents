#pragma once
// tx_pool.h -- the tracker's transmit buffer pool (review HB10, tracker split
// part 1). HUB-ONLY.
//
// A fixed pool of loratx::kPoolSize frame buffers handed out through a
// FreeRTOS queue of free buffers, with a mutex around each buffer's state
// (in_use, ref_count) and integrity checks (bounds, magic). Producers on any
// task get() a buffer, fill it and hand it to the tracker; the send task put()s
// it back. One pool per tracker (it was a file-static shared by every tracker
// in the process; a hub has exactly one).

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_err.h>

#include "esphome/components/lora_client/MacSap.h"

// The transmit buffer pool (review HB25: constants, not global macros).
namespace loratx
{
static constexpr int      kPoolSize    = 5;            // buffers in the TX pool
static constexpr size_t   kBufferSize  = 256;          // one LoRa frame (<= 255 B) fits
static constexpr int      kQueueDepth  = 20;           // TX handoff queue entries
static constexpr uint32_t kBufferMagic = 0xDEADBEEF;   // a pool buffer's integrity mark
}  // namespace loratx
// Memory pool structure with safety features
typedef struct
{
  uint8_t data[loratx::kBufferSize];
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
  // Supersession (txqueue::SupersedeTable). These travel WITH the frame for the
  // same reason the policy above does: the decision is made when the frame
  // reaches the front, by which time the caller that queued it is long gone.
  uint32_t tx_supersede_key;  // 0 = supersedes nothing, superseded by nothing
  uint32_t tx_supersede_gen;
  uint8_t  tx_on_mark;        // TxPolicy::on_mark, stamped into the header
  uint8_t  tx_expects_reply;  // TxPolicy::expects_reply: hold the response window
  // S4: MAC-2's sealer for THIS frame's copies and the binding it is called
  // with (TxPolicy::sealer / seal_binding). Opaque here: MAC-0 never sees a key.
  mac::CopySealer tx_sealer;
  uint64_t        tx_seal_binding;
  uint8_t         tx_fire_stamp;  // TxPolicy::fire_stamp
} tx_buffer_t;

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
    class TxPool
    {
    public:
      TxPool() = default;
      TxPool(const TxPool &) = delete;
      TxPool &operator=(const TxPool &) = delete;

      // Create the free-buffer queue and the mutex and fill the queue.
      esp_err_t init();
      bool ready() const { return this->free_queue_ != nullptr; }
      QueueHandle_t free_queue() const { return this->free_queue_; }
      // A free buffer (thread-safe), or nullptr after `timeout` (counted).
      tx_buffer_t *get(TickType_t timeout);
      // Drop one reference; the last one returns the buffer (thread-safe).
      esp_err_t put(tx_buffer_t *buffer);
      // One more reference (for multiple consumers).
      // In the pool's bounds and carrying the magic.
      bool validate(tx_buffer_t *buffer);
      uint32_t allocation_failures() const { return this->stats_.allocation_failures; }
      UBaseType_t free_count() const { return uxQueueMessagesWaiting(this->free_queue_); }
      // A buffer's index, for the transmit queue's entries, and back.
      uint8_t slot_of(const tx_buffer_t *buffer) const { return (uint8_t) (buffer - this->buffers_); }
      tx_buffer_t *at(uint8_t slot) { return &this->buffers_[slot]; }

    private:
      tx_buffer_t       buffers_[loratx::kPoolSize]{};
      QueueHandle_t     free_queue_{nullptr};
      SemaphoreHandle_t mutex_{nullptr};
      pool_stats_t      stats_{};
    };

  }  // namespace lora_tracker
}  // namespace esphome
