#pragma once
// BenchStreams.h -- the listener's bench streams as classes of their own
// (review HB10, stage 5). HUB-ONLY.
//
// A stream owns its state and its esp_timer and reaches the listener ONLY
// through StreamHost: naming, framing, the FCNT reservations, placement and
// sending, and the ESPHome scheduler. The listener implements StreamHost and
// keeps its public bench API (start_mac_ping, ...) as one-line forwards.
//
// Threading (docs/threading-model.md): the timer callback runs on the
// esp_timer task. It touches only the stream's own state and calls the host's
// timer-safe operations (take_fcnt, frame_with_fcnt, sealed_in_epoch,
// send_placed_from_timer); everything else runs on the loop.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <esp_timer.h>

#include "esphome/components/lora_client/FcntReservation.h"
#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubLinkSecurity.h"

struct MacControl;

namespace esphome
{
  namespace lora_tracker
  {
    struct DeliveryFrame;
    struct TxPolicy;

    // What a bench stream may ask of its listener.
    class StreamHost
    {
    public:
      virtual const char *stream_name() const = 0;
      // A tracker to send through.
      virtual bool stream_has_radio() const = 0;
      virtual bool stream_session_confirmed() const = 0;
      // The listener's placement precondition: grid-aligned and the grid running.
      virtual bool stream_grid_ready() const = 0;
      // Frame `op` under a fresh FCNT from the Sequencer (loop only).
      virtual bool stream_frame_fresh(hubframes::Down *op, DeliveryFrame *out) = 0;
      // Frame `op` under an explicit FCNT (any task).
      virtual bool stream_frame(hubframes::Down *op, uint32_t fcnt, bool encrypt,
                                DeliveryFrame *out) = 0;
      // FCNT reservations (FcntReservation.h): reserve on the loop, take on any task.
      virtual bool stream_reserve_fcnts(uint32_t n, mac1::FcntReservation *r) = 0;
      virtual uint32_t stream_take_fcnt(mac1::FcntReservation *r) = 0;
      virtual bool stream_sealed_in_epoch(const DeliveryFrame &f,
                                          const mac1::FcntReservation &r) = 0;
      // Drop a block under the lock that guards the reservations.
      virtual void stream_invalidate_fcnts(mac1::FcntReservation *r) = 0;
      // The grid, for placing a mark on this node's slot (any task).
      virtual bool stream_grid_started() const = 0;
      virtual int64_t stream_next_t0(int64_t now_us) const = 0;
      virtual int64_t stream_next_clear_t0(int64_t now_us, int copies, size_t len,
                                           bool expects_reply) const = 0;
      // Hand one frame to MAC-0 as it is (any task).
      virtual bool stream_send(const uint8_t *buf, size_t len, const TxPolicy &policy,
                               const mac2::FrameSeal &seal) = 0;
      // Place one frame from the timer task with its own last-mark state.
      virtual bool stream_send_placed(const uint8_t *buf, size_t len, const TxPolicy &policy,
                                      const mac2::FrameSeal &seal, int64_t *last_mark_t0_us) = 0;
      // The ESPHome scheduler (loop only).
      virtual void stream_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) = 0;
      virtual void stream_cancel_timeout(const char *name) = 0;

    protected:
      ~StreamHost() = default;
    };

    // MAC-0 ping / echo (mac-layer.md sections 5 and 6): one placed MacControl
    // PING per tick, echoes counted by seq.
    class MacPingStream
    {
    public:
      // Stage 0 of the funnel is the hub's alone: only the sender knows what it
      // offered. The node can never compute its own frame-error rate, because
      // it cannot know what it did not hear.
      struct Stats
      {
        uint32_t pings_offered{0};   // marks handed to the transmit queue
        uint32_t echoes_rx{0};
        uint32_t echo_seq_gaps{0};   // marks with no echo, by seq
        uint32_t last_seq_sent{0};
        uint32_t last_seq_echoed{0};
        bool     have_echo{false};
      };

      explicit MacPingStream(StreamHost &host) : host_(host) {}
      ~MacPingStream();
      MacPingStream(const MacPingStream &) = delete;
      MacPingStream &operator=(const MacPingStream &) = delete;

      void start(uint32_t duration_s, uint32_t grid_ms, bool want_echo, uint32_t pad_bytes,
                 bool crypto);
      void stop();
      bool active() const { return this->active_; }
      const Stats &stats() const { return this->stats_; }
      void reset_stats() { this->stats_ = Stats{}; }
      void on_echo(const ::MacControl *echo);
      // The host's counter restart / key change: drop the reserved block.
      mac1::FcntReservation *fcnts() { return &this->fcnts_; }
      void tick_for_test() { timer_cb_(this); }

    private:
      void build_frame_();
      static void timer_cb_(void *arg);

      StreamHost &host_;
      std::atomic<bool> active_{false};    // loop writes, timer reads
      std::atomic<bool> cb_busy_{false};   // timer_cb_ running
      // The last mark a ping was placed on (owned by the esp_timer task).
      int64_t  last_mark_t0_us_{0};
      bool     want_echo_{true};
      uint32_t pad_bytes_{0};
      uint32_t seq_{0};
      // Plaintext pings: a counter separate from the session TX id (see
      // build_frame_). Encrypted pings: a block of the session's own FCNTs,
      // reserved in start() on the loop and dropped on every counter restart
      // or key change (FcntReservation.h); guarded by the host's lock.
      uint32_t msgid_{0};
      bool     crypto_{false};
      mac1::FcntReservation fcnts_{};
      esp_timer_handle_t timer_{nullptr};
      uint8_t  frame_[192]{};
      size_t   frame_len_{0};
      mac2::FrameSeal seal_{};
      Stats    stats_{};
    };

    // A stream frame: a fresh FCNT (block == nullptr: START / STOP frames, loop)
    // or the next one of the run's reserved block (marks and ticks, any task).
    // False when the block is spent or dropped, or the keys moved under the seal.
    bool pack_stream_frame(StreamHost &host, hubframes::Down *op, mac1::FcntReservation *block,
                           DeliveryFrame *f);

    // The last ModeTest report, as numbers. Recomputed by the hub from the
    // node's RAW counters (I1: the node never grades itself), stored so HA can
    // plot them rather than have an operator read them out of a log line.
    struct ModeTestSummary
    {
      bool     valid{false};        // false until a report has arrived
      uint32_t mode{0};             // what the node ACTUALLY ran — see
                                    // modetest::modeApplied
      uint32_t arm_refusal{0};
      uint32_t elapsed_s{0};
      uint32_t windows_armed{0};
      uint32_t windows_hit{0};
      uint32_t fer_link_ppm{0};
      uint32_t wmr_ppm{0};
      uint32_t dup_ppm{0};
      uint32_t mic_fail_ppm{0};
      int32_t  phase_p50_us{0};
      int32_t  phase_p99_us{0};
      int32_t  turnaround_p99_us{0};
      bool     power_profile_production{true};
      // The run's clock rate, node against hub. Mode B's pass criterion is
      // stated in these; see implementation-plan.md §12a.
      int32_t  ppm_estimate{0};
      uint32_t ppm_samples{0};
      int32_t  measured_period_us{0};
      // Residual rate after the node clock discipline: Mode B pass line.
      int32_t  residual_ppm{0};
      uint32_t residual_samples{0};
    };

    // ModeTest: the on-hardware validation mode. Every mark carries the full
    // request; in Mode B / sweep the marks are placed on the node's own mark.
    class ModeTestStream
    {
    public:
      explicit ModeTestStream(StreamHost &host) : host_(host) {}
      ~ModeTestStream();
      ModeTestStream(const ModeTestStream &) = delete;
      ModeTestStream &operator=(const ModeTestStream &) = delete;

      void start(uint32_t duration_s, uint32_t grid_ms, uint32_t mode, uint32_t copies,
                 bool keep_power_profile, bool enable_counter, bool enable_crypto,
                 bool mac_echo);
      void stop();
      // send_stop = false is the supervisor's quiet abort.
      void stop_(bool send_stop);
      // One enable=false frame for a node that may still be running a test.
      void send_stop_frame();
      bool active() const { return this->active_; }
      // The running test's request, as the node is told it on every mark.
      uint32_t duration_s() const { return this->duration_s_; }
      uint32_t grid_ms() const { return this->grid_ms_; }
      uint32_t mode() const { return this->mode_; }
      uint32_t copies() const { return this->copies_; }
      bool keep_power_profile() const { return this->keep_power_profile_; }
      bool enable_counter() const { return this->enable_counter_; }
      bool mac_echo() const { return this->mac_echo_; }
      mac1::FcntReservation *fcnts() { return &this->fcnts_; }
      void tick_for_test() { timer_cb_(this); }
      std::vector<uint8_t> frame_for_test() const
      {
        return std::vector<uint8_t>(this->frame_, this->frame_ + this->frame_len_);
      }
      // The node's REPORT: logged, kept as text and as numbers.
      void on_report(const ::ModeTestReport *rep);
      const ModeTestSummary &summary() const { return this->summary_; }
      ModeTestSummary &summary_for_test() { return this->summary_; }

    private:
      void build_frame_(bool enable, mac1::FcntReservation *block = nullptr);
      static void timer_cb_(void *arg);

      StreamHost &host_;
      std::atomic<bool> active_{false};    // loop writes, timer reads
      std::atomic<bool> cb_busy_{false};   // timer_cb_ running; a stop waits for it
      uint32_t duration_s_{0};
      uint32_t grid_ms_{0};
      uint32_t mode_{0};
      uint32_t copies_{1};
      bool     keep_power_profile_{true};
      bool     enable_counter_{false};
      bool     enable_crypto_{false};
      bool     mac_echo_{false};
      uint32_t seq_{0};
      // The last grid mark a Mode B mark was aimed at (owned by the esp_timer
      // task; the loop's last_placed_* are the listener's).
      int64_t  last_mark_t0_us_{0};
      esp_timer_handle_t timer_{nullptr};
      // The run's mark FCNTs, reserved on the loop; guarded by the host's lock.
      mac1::FcntReservation fcnts_{};
      uint8_t  frame_[160]{};
      size_t   frame_len_{0};
      mac2::FrameSeal seal_{};
      ModeTestSummary summary_{};
    };

  }  // namespace lora_tracker
}  // namespace esphome
