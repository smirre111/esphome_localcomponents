#pragma once
// DeliveryService.h -- the MAC-0 delivery service as a class of its own (review
// HB10, split proposal 2). HUB-ONLY.
//
// ONE mechanism for every hub->node application frame: the Control, Config and
// Mac channels (one request in flight each, acked, with a retry ladder) and
// Fire requests (sent once, no channel). The service owns the slots, their ack
// ids, the ladder and its timers; the decisions are DeliveryPolicy.h's. It
// reaches its listener ONLY through DeliveryHost: framing, placement, the
// session's state, the node's link timing belief and the ESPHome scheduler.
// Loop task only (docs/threading-model.md).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "esphome/components/lora_client/DeliveryPolicy.h"
#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubLinkSecurity.h"
#include "esphome/components/lora_client/SingleShotAckWait.h"

namespace esphome
{
  namespace lora_tracker
  {

    // A request for the generic delivery glue (DeliveryService::deliver): what to
    // send, what to do when it is acknowledged, what to do when it is given up.
    // The retry rules are the Policy (DeliveryPolicy.h); the callbacks are the
    // only part that knows what the frame is.
    // ---- S7: the MAC-0 delivery service's contract with the application ----
    //
    // The service (DeliveryService below, policy in DeliveryPolicy.h)
    // owns everything ARQ: the msgid of every attempt, the header and the frame,
    // the pack-once copy a Resend::SameBytes request re-sends byte for byte, the
    // ack matching, the retry ladder and the timers. The application only says
    // what an attempt carries (encode), where it goes on the air (place), and is
    // told the outcome (on_delivered / on_fail / on_abandon).

    // One framed attempt, as the service hands it to `place`. `msgid` is the
    // frame's id; the application may print it, never keep or compare it.
    struct DeliveryFrame
    {
      std::vector<uint8_t> bytes;
      mac2::FrameSeal      seal{};    // sealed=false: plaintext
      uint32_t             msgid{0};  // set even when packing failed (it was allocated)
    };
    struct DeliveryAttempt
    {
      bool     retry{false};          // a retry of the request (counted or a burst fallback)
      bool     resent_stored{false};  // the stored first frame again (Resend::SameBytes)
      uint32_t tag{0};                // what encode said the attempt stands for
    };
    // The node answered an attempt. `tag` is that attempt's tag; `log_id` the id
    // the node acknowledged, for log lines only.
    struct DeliveryConfirm
    {
      uint32_t tag{0};
      uint32_t log_id{0};
    };

    struct DeliveryRequest
    {
      const char *what{"request"};          // for the log
      const char *resend_verb{"resending"}; // how a counted retry is logged; nullptr: the service's SameBytes lines
      // Resend::SameBytes only: "<label> sent (msgid=N) — awaiting ack" on the first attempt.
      const char *sent_label{nullptr};
      deliverypolicy::Policy policy{};
      // Say what ONE attempt carries: fill a message on YOUR stack (payload only)
      // and hand it to `frame`, which adds the header and a fresh msgid, packs and
      // seals it; return what `frame` returned (false: nothing was framed). `*tag`
      // names what the attempt stands for (a ScheduleConfig's version) and comes
      // back in on_delivered. Asked for every attempt, except that a
      // Resend::SameBytes request's retries re-send its first frame unchanged.
      using FrameFn = std::function<bool(hubframes::Down *op)>;
      std::function<bool(bool retry, uint32_t *tag, const FrameFn &frame)> encode;
      // Put a framed attempt on the air (placement, shape, the bookkeeping that
      // depends on where it went). Empty: framed but not transmitted.
      std::function<void(const DeliveryFrame &f, const DeliveryAttempt &a)> place;
      // The node answered an attempt. True when that completes the request
      // (false: it answered a superseded attempt and the newer one is still awaited).
      std::function<bool(const DeliveryConfirm &c)> on_delivered;
      // The ladder is spent.
      std::function<void()> on_fail;
      // The request has left its channel (acked, or given up). Runs last.
      std::function<void()> on_done;
      // Whether a retry may go out at all (an at-most-once request that can no
      // longer be re-sent byte for byte must be failed instead). Empty: always.
      std::function<bool()> resendable;
      // resendable() said no: the request is failed WITHOUT another send. Empty:
      // on_fail runs instead.
      std::function<void()> on_abandon;
      // ---- Fire requests only (Policy::mode == Fire): sent once, no ack ----
      // Whether the frame is wanted at all, asked after the relogin deferral and
      // before a msgid is taken. False: dropped (the callback logs why).
      std::function<bool()> admit;
      // Fill in the command (cmd_case and its payload); the header and the msgid
      // are the mechanism's. The payload must outlive the call (own it in a capture).
      std::function<void(hubframes::Down *op)> fill;
      // The frame is on the air (or queued for it); `log_id` is its id, for log lines only.
      std::function<void(uint32_t log_id)> on_sent;
    };


    // The delivery channels. A request occupies one channel until it is acked,
    // fails, or is superseded; channels do not block each other.
    enum DeliveryChannel : uint8_t
    {
      CH_CONTROL = 0,   // cover operations and sysops
      CH_CONFIG  = 1,   // ScheduleConfig
      CH_MAC     = 2,   // GridSync (enable)
      CH_SETUP   = 3,   // ClientConfig / CoverConfig, acked (one at a time, in order)
      kDeliveryChannels = 4,
      // A Fire request (TimeSync, ClientConfig, CoverConfig) occupies no channel: it
      // is sent once and forgotten, and never touches a slot.
      CH_NONE    = 0xFF
    };

    // What the delivery service may ask of its listener.
    class DeliveryHost
    {
    public:
      virtual const char *dlv_name() const = 0;
      // A tracker to send through.
      virtual bool dlv_has_radio() const = 0;
      // A relogin is under way: no session to seal an attempt under.
      virtual bool dlv_relogin_pending() const = 0;
      // Frame `op` under a fresh FCNT (header, pack, seal).
      // `ack_req`: ask for a MAC ack whatever the route table says (an Acked
      // request's frame must be answerable).
      virtual bool dlv_frame(hubframes::Down *op, DeliveryFrame *out, bool ack_req) = 0;
      // Put a Fire request's frame on the air the way `shape` says.
      virtual bool dlv_place_fire(deliverypolicy::Shape shape, const DeliveryFrame &f,
                                  const char *what) = 0;
      // A request that a Mode B warm-up must not run into is starting.
      virtual void dlv_yield_warmup() = 0;
      // The transmit queue's drain estimate (0 without a radio).
      virtual int64_t dlv_tx_drain_us() const = 0;
      // The mark the last placed frame went out on.
      virtual int64_t dlv_last_placed_t0_us() const = 0;
      // The node's mark->ack history (SingleShotAckWait.h).
      virtual singleshotwait::AckLatency &dlv_ack_latency() = 0;
      // Rule 4: a single shot went unacked; bursts until the node is confirmed.
      virtual bool dlv_single_shot_unacked() const = 0;
      virtual void dlv_mark_single_shot_unacked() = 0;
      // A request ran out of retries with FailRelogin: the session is dead.
      virtual void dlv_drop_session() = 0;
      virtual void dlv_relogin() = 0;
      // The ESPHome scheduler.
      virtual void dlv_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) = 0;
      virtual void dlv_cancel_timeout(const char *name) = 0;

    protected:
      ~DeliveryHost() = default;
    };

    class DeliveryService
    {
      struct DeliverySlot;   // one channel's request in flight (private, below)

    public:
      // The most an ack timer is stretched by the transmit backlog. A ceiling
      // rather than trust: the drain estimate is written from two cores without
      // a lock, and a torn read must not park a retry for hours.
      static constexpr uint32_t kMaxAckBacklogMs = 60000;

      explicit DeliveryService(DeliveryHost &host) : host_(host) {}
      DeliveryService(const DeliveryService &) = delete;
      DeliveryService &operator=(const DeliveryService &) = delete;

      // Start `req` on channel `ch`, superseding one already in flight there (its
      // ack ids stay recognised). A Fire request (CH_NONE) is sent once.
      void deliver(DeliveryChannel ch, DeliveryRequest req);
      // The node acknowledged `ack_msg_id`: true when a request owns it.
      bool ack(uint32_t ack_msg_id);
      // An ack whose status says the node will not execute the request (P3).
      bool refused(uint32_t ack_msg_id, AckStatus status);
      // Forget the request on `ch`: its timer, its wait and its ack ids.
      void cancel(DeliveryChannel ch);
      // A new session restarts the msgid space: ids on record would alias new ones.
      void forget_ids();
      // `base_ms` plus the transmit backlog (see ack_wait_ms).
      uint32_t ack_wait_ms(uint32_t base_ms) const;
      // (Re-)arm the channel's ack wait.
      void arm_timer(DeliveryChannel ch);
      static const char *timer_name(DeliveryChannel ch);

      // ---- the channels' delivery state: read-only outside the service ----
      // Only const references are handed out (slot_for_test aside): callers
      // read a slot's fields and change it through the methods below.
      using SlotState = DeliverySlot;
      const SlotState &slot(DeliveryChannel ch) const { return this->slots_[ch]; }
      // The few things the application may change about a channel's request.
      // The node acknowledged: no longer awaited (the ack path then cancels).
      void clear_awaiting(DeliveryChannel ch) { this->slots_[ch].awaiting_ack = false; }
      // Ack ids on record: a superseded or given-up request's must not complete it.
      void forget_ack_ids(DeliveryChannel ch) { this->slots_[ch].acks.clear(); }
      // The stored frame was sealed under keys just destroyed: never re-send it.
      void drop_stored_frame(DeliveryChannel ch)
      {
        this->slots_[ch].frame     = DeliveryFrame{};
        this->slots_[ch].frame_tag = 0;
      }
      // The shape the request's frame went out in (Rule 4, the short first wait).
      void set_sent_shape(DeliveryChannel ch, bool single_shot, bool optimistic)
      {
        this->slots_[ch].sent_single_shot = single_shot;
        this->slots_[ch].sent_optimistic  = optimistic;
      }
      // The mark the request's FIRST frame was placed on (0 = not a single placed copy).
      void set_mark_us(DeliveryChannel ch, int64_t mark_us) { this->slots_[ch].mark_us = mark_us; }
      // A new command on the channel may use the short first wait again.
      void reset_short_wait(DeliveryChannel ch) { this->slots_[ch].short_wait_spent = false; }

      // For tests: set up a channel's delivery state directly.
      DeliverySlot &slot_for_test(DeliveryChannel ch) { return this->slots_[ch]; }

    private:
      bool     fire_(DeliveryRequest &req);
      bool     tx_(DeliveryChannel ch, bool retry);
      uint32_t attempt_(DeliverySlot &s, bool retry, uint32_t *tag);
      void     log_attempt_(const DeliverySlot &s, bool retry, uint32_t msgid);
      uint32_t short_wait_ms_(DeliveryChannel ch);
      void     on_timeout_(DeliveryChannel ch, bool short_wait);
      void     resend_(DeliveryChannel ch);
      void     abandon_(DeliveryChannel ch);
      void     short_wait_missed_(DeliveryChannel ch);
      void     give_up_(DeliveryChannel ch);
      // Run the channel's on_done (a copy: it may start the channel's next request).
      void     done_(DeliveryChannel ch);

      // ONE DELIVERY CHANNEL'S REQUEST IN FLIGHT (step 2 of the unified MAC
      // delivery refactor). Everything the tracked-command machinery used to keep
      // as loose op_* members on the listener. One request per channel at a time
      // (Control: cover operations and sysops, with a small line of what waits
      // behind it; Config: ScheduleConfig; Mac: GridSync).
      struct DeliverySlot
      {
        bool     awaiting_ack{false};
        uint32_t first_msgid{0};   // first msgid used for the current logical command
        uint32_t last_msgid{0};    // msgid of the most recent (re)transmission
        uint8_t  retry_count{0};
        // Resend::SameBytes: the first attempt's frame (bytes, msgid, and the keys
        // it was sealed under -- a retry hands the SAME binding to the tracker, so
        // a frame sealed before a re-key is never re-tagged, F4), and its tag.
        // Owned by the delivery service (S7).
        DeliveryFrame        frame{};
        uint32_t             frame_tag{0};
        // Whether the frame currently awaiting an ack went out as a single copy.
        // Rule 4 bounds the exposure to ONE frame: the first retry of a single
        // shot puts the node back on bursts until it is confirmed again.
        bool     sent_single_shot{false};
        // The command's first shot was an optimistic single shot, so its first ack
        // wait is the short one (SingleShotAckWait.h).
        bool     sent_optimistic{false};
        // The short wait has been used for this command. It is used at most once,
        // and is never counted against kOpMaxRetries.
        bool     short_wait_spent{false};
        // The mark the command's FIRST frame was placed on, 0 when that frame was
        // not a single placed copy. Captured at send time: other placements move
        // last_placed_t0_us_ before the ack arrives.
        int64_t  mark_us{0};
        // The tail (ms after the mark) the first ack wait used; 0 = none.
        uint32_t first_tail_ms{0};
        // ---- the generic glue (step 3): Config and Mac channels ----
        // awaiting_ack / retry_count above are shared with it. The ids are every
        // msgid the current AND earlier requests went out under, with the tag of
        // the attempt: an ack for any of them is recognised.
        DeliveryRequest        req;
        deliverypolicy::AckIds acks;
      };

      DeliveryHost &host_;
      DeliverySlot  slots_[kDeliveryChannels];
    };

  }  // namespace lora_tracker
}  // namespace esphome
