#pragma once
// tx_scheduler.h -- the tracker's transmit scheduler (review HB10, tracker
// split part 2). HUB-ONLY.
//
// Three things decide when and whether a frame goes out, and they live here:
//   * the ORDER: the reordering, time-scheduled queue of pool slots
//     (txqueue::Queue) -- touched by the send task only;
//   * SUPERSESSION: which command generation is current per node
//     (txqueue::SupersedeTable) -- a frame whose generation is stale is dropped
//     when it reaches the front;
//   * the CHANNEL CALENDAR: when the burst on the air ends, when the frames
//     placed but not yet sent end, when the last accepted frame will have left
//     (the queue's drain), and every reserved span, so a placement can find a
//     mark that is clear of the hub's own air.
// The supersession table and the calendar are written by send() on the loop
// and esp_timer tasks and by the send task, and the 64-bit fields tear on the
// 32-bit core: every access holds mutex_ (HB2). Nothing logs or blocks under it;
// the end-of-burst arithmetic the templates call is pure.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "esphome/components/lora_client/TxQueue.h"

namespace esphome
{
  namespace lora_tracker
  {
    class TxScheduler
    {
    public:
      // Eight covers the pool (loratx::kPoolSize) plus the burst on air; a full
      // table evicts the span that ends first.
      static constexpr size_t kMaxReservations = 8;

      // ---- the order (send task only) ----
      bool    push(uint8_t slot, int64_t earliest_us, txqueue::Priority priority)
      {
        return this->queue_.push(slot, earliest_us, priority);
      }
      uint8_t pop_due(int64_t now_us, int64_t lead_us, int64_t &earliest_us_out)
      {
        return this->queue_.popDue(now_us, lead_us, earliest_us_out);
      }
      int64_t next_eligible_us(int64_t now_us) const { return this->queue_.nextEligibleUs(now_us); }
      uint8_t queued() const { return this->queue_.size(); }

      // ---- supersession ----
      // Recorded when a frame is ACCEPTED, so a second command queued while the
      // first still waits raises the mark at once.
      void note_generation(uint32_t key, uint32_t gen)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->supersede_.note(key, gen);
      }
      bool is_current(uint32_t key, uint32_t gen) const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return this->supersede_.isCurrent(key, gen);
      }

      // ---- the channel calendar ----
      int64_t busy_until_us() const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return this->burst_busy_until_us_;
      }
      int64_t placed_busy_until_us() const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return this->placed_busy_until_us_;
      }
      // How long until every accepted frame has left the radio.
      int64_t drain_us(int64_t now_us) const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        int64_t end_us = this->queued_air_end_us_;
        if (this->burst_busy_until_us_ > end_us)
          end_us = this->burst_busy_until_us_;
        return (end_us > now_us) ? end_us - now_us : 0;
      }
      // The first instant clear of the burst on the air and of every placed frame.
      int64_t clear_floor_us(int64_t now_us) const
      {
        int64_t floor_us = now_us;
        std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->burst_busy_until_us_ > floor_us)  floor_us = this->burst_busy_until_us_;
        if (this->placed_busy_until_us_ > floor_us) floor_us = this->placed_busy_until_us_;
        return floor_us;
      }
      // Price an accepted frame into the queue: it leaves after whatever is on
      // the air and after every frame accepted before it.
      // end_from_start(start_us) -> when it stops occupying the channel.
      template <class EndFn>
      void price_accepted(int64_t now_us, int64_t earliest_us, EndFn end_from_start)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        int64_t start_us = this->queued_air_end_us_;
        if (this->burst_busy_until_us_ > start_us) start_us = this->burst_busy_until_us_;
        if (now_us > start_us)                     start_us = now_us;
        if (earliest_us > start_us)                start_us = earliest_us;
        this->queued_air_end_us_ = end_from_start(start_us);
      }
      // A PLACED frame reserves its span from the moment it is accepted.
      void reserve_placed(int64_t start_us, int64_t end_us)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        if (end_us > this->placed_busy_until_us_)
          this->placed_busy_until_us_ = end_us;
        this->reserve_locked_(start_us, end_us);
      }
      // The frame going on the air now: the channel is busy until it ends. A
      // placed frame reserved its span when accepted; an unplaced one only now.
      template <class EndFn>
      void begin_burst(int64_t start_us, bool placed, EndFn end_from_start)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->burst_busy_until_us_ = end_from_start(start_us);
        if (!placed)
          this->reserve_locked_(start_us, this->burst_busy_until_us_);
      }
      // Walk `t0` forward a round at a time (at most max_rounds) to the first
      // mark whose span (span_of(t0, &start, &end)) overlaps no live
      // reservation -- and, for a burst (`respect_listen`), no live listen
      // window. False: none found; t0 is then max_rounds rounds on.
      template <class SpanFn>
      bool first_clear(int64_t &t0, int64_t now_us, uint32_t max_rounds, int64_t round_us,
                       SpanFn span_of, bool respect_listen = false) const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        for (uint32_t n = 0; n < max_rounds; ++n, t0 += round_us)
        {
          int64_t start = 0, end = 0;
          span_of(t0, &start, &end);
          if (!overlaps_locked_(this->reservations_, now_us, start, end) &&
              !(respect_listen && overlaps_locked_(this->listen_, now_us, start, end)))
            return true;
        }
        return false;
      }

      // ---- listen windows: a reply another node owes the hub ----
      // The hub cannot receive while it transmits. A node that was sent ONE
      // placed copy of a tracked command acks it inside [start, end); a burst
      // for any other node started in that window would make the hub deaf to
      // the ack (2026-11-01 23:00: node 2's SLEEP ack lost in node 1's retry
      // burst). Bursts are placed past live windows; single copies are not held
      // (one copy is as short as the ack it might meet). One window per node
      // (`key`); a full table evicts the window that ends first.
      static constexpr size_t kMaxListenWindows = 8;
      void reserve_listen(uint32_t key, int64_t start_us, int64_t end_us)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        size_t slot = 0;
        for (size_t i = 0; i < kMaxListenWindows; ++i)
        {
          if (this->listen_keys_[i] == key)
          {
            slot = i;
            break;
          }
          if (this->listen_[i].end_us < this->listen_[slot].end_us)
            slot = i;
        }
        this->listen_keys_[slot]     = key;
        this->listen_[slot].start_us = start_us;
        this->listen_[slot].end_us   = end_us;
      }
      void release_listen(uint32_t key)
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        for (size_t i = 0; i < kMaxListenWindows; ++i)
          if (this->listen_keys_[i] == key)
          {
            this->listen_keys_[i] = 0;
            this->listen_[i]      = Reservation{};
          }
      }
      // The first start >= start_us at which a span of dur_us overlaps no live
      // listen window.
      int64_t after_listen(int64_t start_us, int64_t dur_us, int64_t now_us) const
      {
        std::lock_guard<std::mutex> lock(this->mutex_);
        for (size_t pass = 0; pass <= kMaxListenWindows; ++pass)
        {
          bool moved = false;
          for (const Reservation &w : this->listen_)
          {
            if (w.end_us > now_us && start_us < w.end_us && w.start_us < start_us + dur_us)
            {
              start_us = w.end_us;
              moved    = true;
            }
          }
          if (!moved)
            break;
        }
        return start_us;
      }

    private:
      struct Reservation
      {
        int64_t start_us{0};
        int64_t end_us{0};
      };
      template <size_t N>
      static bool overlaps_locked_(const std::array<Reservation, N> &spans, int64_t now_us,
                                   int64_t start, int64_t end)
      {
        for (const Reservation &r : spans)
          if (r.end_us > now_us && start < r.end_us && r.start_us < end)
            return true;
        return false;
      }
      void reserve_locked_(int64_t start_us, int64_t end_us)   // caller holds mutex_
      {
        size_t victim = 0;
        for (size_t i = 1; i < kMaxReservations; ++i)
          if (this->reservations_[i].end_us < this->reservations_[victim].end_us)
            victim = i;
        this->reservations_[victim].start_us = start_us;
        this->reservations_[victim].end_us   = end_us;
      }

      // B1a: the reordering, time-scheduled transmit queue (send task only).
      txqueue::Queue queue_;
      mutable std::mutex mutex_;
      // Which generation is current for each node.
      txqueue::SupersedeTable supersede_;
      // When the hub's own burst stops occupying the channel, including the
      // post-burst response window the send task holds the radio in.
      int64_t burst_busy_until_us_{0};
      // The same for frames placed but not yet dequeued.
      int64_t placed_busy_until_us_{0};
      // When the last frame accepted by send() will have left the radio. Grows by
      // one burst per accepted frame, so it prices a QUEUE where
      // burst_busy_until_us_ only knows the frame currently on the air.
      int64_t queued_air_end_us_{0};
      std::array<Reservation, kMaxReservations> reservations_{};
      std::array<Reservation, kMaxListenWindows> listen_{};
      std::array<uint32_t, kMaxListenWindows>    listen_keys_{};
    };

  }  // namespace lora_tracker
}  // namespace esphome
