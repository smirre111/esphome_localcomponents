// lora_client_delivery.cpp -- the listener's MAC delivery service: ONE
// mechanism for every hub->node application frame (deliver_ / dlv_* over the
// Control, Config and Mac channels, plus Fire requests), the Control channel's
// command queue (ctl_*), and the ack / refusal handling. The decisions are
// DeliveryPolicy.h's; this performs them. Stage 5 split of lora_client.cpp,
// moved verbatim. Loop task only (docs/threading-model.md).
#include "lora_client_internal.h"

namespace esphome
{
  namespace lora_tracker
  {

    // The tracked command's payload (S7): built from the slot's command. The
    // delivery service frames it under a fresh msgid, keeps the frame (pack once,
    // Resend::SameBytes) and re-sends THOSE bytes on every retry.
    //
    // B4: PACK ONCE. Re-packing was two bugs at once. It minted a fresh msgid, so
    // the node's replay filter admitted the retry and executed the command a
    // SECOND time — a blind that moves twice. And re-reading the position at
    // pack time meant a user moving the blind mid-retry produced different
    // plaintext under a msgid-derived CTR counter block; with a reused msgid that
    // is keystream reuse, and with a fresh one it is a duplicate command. Storing
    // the frame removes both: same msgid, same ciphertext, and the node can
    // recognise a duplicate and re-ack it.
    bool LORAListener::encode_ctl_(const DeliveryRequest::FrameFn &frame)
    {
      hubframes::Down op_message {};
      LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
      if (this->ctl().kind == TrackedOpKind::SYSOP)
      {
        op_message.cmd_case = hubframes::DL_SYSOP;
        op_message.sysop    = static_cast<ClientOperation>(this->ctl().sysop);
      }
      else
      {
        op_message.cmd_case = hubframes::DL_OPERATION;
        if (this->ctl().covop_case == LORA_COVER_OPERATION__COVOP_POSITION)
        {
          covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
          covop.position   = this->ctl().position;
        }
        else
        {
          covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
          covop.operation  = static_cast<CovOperation>(this->ctl().operation);
        }
        op_message.operation = &covop;
      }
      // Encrypt the command in-session (CTR + CMAC) so the downlink op is
      // authenticated; falls back to plaintext only before a session exists.
      if (frame(&op_message))
        return true;
      this->ctl().mark_us = 0;
      ESP_LOGE(TAG, "[%s] Failed to pack tracked operation", this->get_name().c_str());
      return false;
    }

    // Where the tracked command goes: aligned, under its supersession generation
    // (a retransmission stays under the generation it belongs to, or a retry
    // would supersede the frame it is a retry of).
    void LORAListener::place_ctl_(const MacFrame &f, const DeliveryAttempt &a)
    {
      // The tracked command downlink is the one B1's gate is about: with
      // alignment on, this is the burst that must observably start on the
      // grid. With it off (the default) behaviour is byte-for-byte as before.
      this->send_aligned_(f.bytes.data(), f.bytes.size(), this->tracked_op_policy_(), f.seal);
      // Rule 4's exposure is THIS command's, so the shape is recorded here
      // rather than inside send_aligned_, where every other producer would
      // overwrite it.
      this->ctl().sent_single_shot = this->last_placed_single_shot_;
      this->ctl().sent_optimistic  = this->last_placed_optimistic_;
      // The placed mark of a FRESH frame (the first, or a re-pack) times the
      // first ack; a re-send of the stored frame leaves it alone.
      if (!a.resent_stored)
        this->ctl().mark_us = this->last_placed_single_shot_ ? this->last_placed_t0_us_ : 0;
    }

    // The Control channel's request: the command the slot describes, delivered by
    // the generic glue. The decisions are the policy's (DeliveryPolicy.h); what is
    // specific to a tracked command is here: the stored frame is re-sent byte for
    // byte, an optimistic single shot gets the short first wait, and a sleep that
    // goes unanswered is not a dead session.
    DeliveryRequest LORAListener::ctl_request_()
    {
      static_assert(deliverypolicy::kControlMaxRetries == kOpMaxRetries &&
                        deliverypolicy::kControlWaitMs == kOpRetryIntervalMs,
                    "the Control policies are the verified ladder");
      DeliveryRequest req;
      req.what        = "Tracked op";
      req.resend_verb = nullptr;   // the service's SameBytes lines log the retransmit, with the frame id
      req.sent_label  = this->ctl().kind == TrackedOpKind::SYSOP ? "Sysop" : "Cover op";
      req.policy      = deliverypolicy::controlPolicy(
          this->ctl().kind == TrackedOpKind::COVER, this->ctl().sysop,
          CLIENT_OPERATION__CMD_SLEEP, CLIENT_OPERATION__CMD_OTA);
      req.encode  = [this](bool, uint32_t *, const DeliveryRequest::FrameFn &frame) {
        return this->encode_ctl_(frame);
      };
      req.place        = [this](const MacFrame &f, const DeliveryAttempt &a) { this->place_ctl_(f, a); };
      req.on_delivered = [this](const DeliveryConfirm &c) { return this->on_ctl_acked_(c.log_id); };
      req.on_fail = [this]() { this->on_ctl_failed_(); };
      // An at-most-once command (OTA) whose stored frame is gone -- a relogin
      // clears it, and the session it was sealed under is dead -- must not be
      // re-packed: that would be a NEW command, and the node may have run the
      // first one already.
      req.resendable  = [this]() {
        return !(this->ctl().req.policy.at_most_once && this->ctl().frame.bytes.empty());
      };
      req.on_abandon  = [this]() { this->on_ctl_abandoned_(); };
      req.on_done     = [this]() { this->ctl_next_(); };
      return req;
    }

    // Start the slot's command: one attempt now, then the ladder.
    void LORAListener::ctl_send_()
    {
      this->ctl().deferred_until_login = false;
      this->ctl().short_wait_spent     = false;
      // A superseded command's ack must not complete this one.
      this->ctl().acks.clear();
      this->set_command_failed_(false);
      this->deliver_(CH_CONTROL, this->ctl_request_());
    }

    bool LORAListener::on_ctl_acked_(uint32_t ack_msg_id)
    {
      this->ctl().awaiting_ack = false;
      this->set_command_failed_(false);
      ESP_LOGI(TAG, "[%s] Tracked op acknowledged (ack_msg_id=%u)", this->get_name().c_str(),
               (unsigned) ack_msg_id);
      if (this->ctl().mark_us != 0)
        this->record_ack_latency_();
      // The nightly sleep takes effect on the hub only now: the node has heard it.
      if (this->op_is_sleep_())
        this->on_sleep_acknowledged_();
      return true;
    }

    // A retry was due but the command cannot be re-sent safely: failed, nothing sent.
    // The session is NOT torn down (the relogin that cleared the frame is the
    // session change; a second one would only repeat it).
    void LORAListener::on_ctl_abandoned_()
    {
      this->ctl().acks.clear();
      this->set_command_failed_(true);
    }

    // Every retry was spent without an ack. (The policy decides whether the
    // session is torn down: not for a sleep, whose silence is ambiguous, see
    // on_sleep_unconfirmed_().)
    void LORAListener::on_ctl_failed_()
    {
      // A late ack of a command given up on must not revive it.
      this->ctl().acks.clear();
      if (this->op_is_sleep_())
      {
        this->on_sleep_unconfirmed_();
        return;
      }
      ESP_LOGE(TAG, "[%s] Tracked op not acknowledged after %u retries — marking failed",
               this->get_name().c_str(), (unsigned) kOpMaxRetries);
      this->set_command_failed_(true);
    }

    bool LORAListener::send_cover_operation(uint32_t covop_case, int32_t operation, float position)
    {
      return this->ctl_submit_({true, covop_case, operation, position, 0});
    }

    void LORAListener::send_tracked_sysop_(int32_t sysop)
    {
      // P1: one-shot sysops used to be sent once, unacked -- a single dropped or
      // replay-rejected frame was lost with no recovery. They go through the same
      // tracked machinery as cover ops: a miss is retransmitted until the node
      // acks, then fails + re-logins.
      this->ctl_submit_({false, 0, 0, 0.0f, sysop});
    }

    bool LORAListener::ctl_busy_() const
    {
      return this->ctl().awaiting_ack || this->ctl().deferred_until_login;
    }

    // One command in flight per node. A newer cover op replaces an older one; any
    // other request waits its turn rather than overwriting what is in flight (a
    // SLEEP, an OTA or a mode switch is never superseded -- and the overwritten
    // SLEEP used to lose its own ack).
    // Returns false when the command was IGNORED because the node sleeps.
    bool LORAListener::ctl_submit_(const deliverypolicy::ControlCmd &c)
    {
      if (this->refuse_while_asleep_(c))
        return false;
      this->mbs_yield_to_op_();
      if (deliverypolicy::admitControl(this->ctl_busy_(), this->ctl().kind == TrackedOpKind::COVER,
                                       c.is_cover) == deliverypolicy::ControlAdmit::Queue)
      {
        this->ctl_enqueue_(c);
        return true;
      }
      this->ctl_begin_(c, false);
      return true;
    }

    void LORAListener::ctl_begin_(const deliverypolicy::ControlCmd &c, bool hold)
    {
      if (c.is_cover)
      {
        this->ctl().kind       = TrackedOpKind::COVER;
        this->ctl().covop_case = c.covop_case;
        this->ctl().operation  = c.operation;
        this->ctl().position   = c.position;
      }
      else
      {
        this->ctl().kind  = TrackedOpKind::SYSOP;
        this->ctl().sysop = c.sysop;
      }
      // A NEW logical command, so a previous one still sitting in the transmit
      // queue is now stale. Bumped before the pack, so the frame carries the
      // generation that supersedes it. See txqueue::SupersedeTable.
      this->ctl().generation++;

      // Security review finding 1: see deferred_until_login's comment. The
      // command's parameters are already stored in the slot, so
      // confirm_session_() can send exactly this once encryption is possible,
      // rather than this sending a plaintext frame now that the node (already
      // holding a session) will reject outright.
      if (this->relogin_pending_ || hold)
      {
        this->ctl().deferred_until_login = true;
        ESP_LOGD(TAG, "[%s] %s deferred — %s", this->get_name().c_str(),
                 c.is_cover ? "Cover op" : "Sysop",
                 hold ? "the node is asleep" : "session being rebuilt");
        return;
      }
      this->ctl_send_();
    }

    void LORAListener::ctl_enqueue_(const deliverypolicy::ControlCmd &c)
    {
      const char *what = c.is_cover ? "Cover op" : "Sysop";
      switch (this->ctl_queue_.push(c))
      {
      case deliverypolicy::ControlQueue::Push::Full:
        // The newest is refused, never an older one: what is queued was asked for.
        ESP_LOGE(TAG, "[%s] %s refused — %u commands are already waiting behind the one in flight",
                 this->get_name().c_str(), what, (unsigned) deliverypolicy::ControlQueue::kDepth);
        this->set_command_failed_(true);
        return;
      case deliverypolicy::ControlQueue::Push::ReplacedCover:
        ESP_LOGI(TAG, "[%s] %s queued behind the command in flight — replaces the cover op already waiting",
                 this->get_name().c_str(), what);
        return;
      case deliverypolicy::ControlQueue::Push::Queued:
        ESP_LOGI(TAG, "[%s] %s queued behind the command in flight (%u waiting)",
                 this->get_name().c_str(), what, (unsigned) this->ctl_queue_.size());
        return;
      }
    }

    void LORAListener::ctl_next_()
    {
      deliverypolicy::ControlCmd c;
      if (!this->ctl_queue_.pop(&c))
        return;
      ESP_LOGI(TAG, "[%s] Starting the %s that waited", this->get_name().c_str(),
               c.is_cover ? "cover op" : "sysop");
      this->ctl_begin_(c, this->ctl_hold_for_wake_);
    }

    TxPolicy LORAListener::tracked_op_policy_() const
    {
      TxPolicy p;
      p.supersede_key = this->short_address_;
      p.supersede_gen = this->ctl().generation;
      return p;
    }

    const char *LORAListener::dlv_timer_(DeliveryChannel ch)
    {
      static const char *const kNames[kDeliveryChannels] = {"dlv_ctl", "dlv_cfg", "dlv_mac"};
      return kNames[ch];
    }

    void LORAListener::deliver_(DeliveryChannel ch, DeliveryRequest req)
    {
      // A Fire request is sent once and occupies no channel: it never reaches the
      // slot below, so it cannot cancel, replace or delay a request in flight on any
      // channel (a TimeSync goes out between a ScheduleConfig's attempts).
      if (req.policy.mode == deliverypolicy::Mode::Fire)
      {
        this->dlv_fire_(req);
        return;
      }
      if (ch >= kDeliveryChannels)
        return;   // an acked request needs a channel to wait on
      DeliverySlot &s = this->slots_[ch];
      // A request already in flight is REPLACED by this one, with a fresh ladder
      // of its own (the budget is per request). Its msgids stay on record in
      // s.acks, so a late ack for the old request is still recognised. Its timer
      // needs no cancelling: the first attempt re-arms the same named one, and a
      // request that never arms one leaves it to find nothing awaited.
      s.req          = std::move(req);
      s.retry_count  = 0;
      s.awaiting_ack = false;
      // The stored frame belongs to the request being replaced. A new request
      // whose first pack fails must re-pack (or, at-most-once, be abandoned) on
      // its retry, never re-send the previous command's bytes (review HB3).
      s.frame        = MacFrame{};
      s.frame_tag    = 0;
      if (s.req.policy.yields_warmup)
        this->mbs_yield_self_();
      this->dlv_tx_(ch, false);
    }

    // THE one transmission of a Fire request (step 6): TimeSync, ClientConfig,
    // CoverConfig. Everything that used to be repeated at each of those three call
    // sites lives here once -- the relogin deferral, the msgid, the header, the
    // encryption decision (v1_frame_: sealed whenever a session is confirmed,
    // plaintext only before one exists), the shape and the failure log. What the
    // frame SAYS, and when it is wanted at all, is the request's.
    bool LORAListener::dlv_fire_(DeliveryRequest &req)
    {
      if (this->parent_ == nullptr || !req.fill)
        return false;   // no radio, or nothing to say
      // See relogin_pending_. Checked before the msgid is taken, so a deferred
      // frame does not burn one; confirm_session_ sends it again.
      if (deliverypolicy::fireDeferred(req.policy, this->relogin_pending_))
      {
        ESP_LOGD(TAG, "[%s] %s deferred — session being rebuilt", this->get_name().c_str(),
                 req.what);
        return false;
      }
      if (req.admit && !req.admit())
        return false;

      hubframes::Down op_message {};
      req.fill(&op_message);

      MacFrame f;
      if (!this->mac_frame_(&op_message, &f))
      {
        ESP_LOGE(TAG, "[%s] Failed to pack %s", this->get_name().c_str(), req.what);
        return false;
      }
      const bool sent = this->fire_place_(req.policy.shape, f, req.what);
      if (sent && req.on_sent)
        req.on_sent(f.msgid);
      return sent;
    }

    bool LORAListener::fire_place_(deliverypolicy::Shape shape, const MacFrame &f, const char *what)
    {
      const uint8_t *buf = f.bytes.data();
      const size_t   len = f.bytes.size();
      switch (shape)
      {
      case deliverypolicy::Shape::Bare:
        return this->mac0_send_(buf, len, TxPolicy{}, f.seal);
      case deliverypolicy::Shape::Auto:
        return this->send_aligned_(buf, len, f.seal);
      case deliverypolicy::Shape::Burst:
      {
        TxPolicy p;
        p.copies = this->parent_->defaultBurstCopies();
        return this->send_aligned_(buf, len, p, f.seal);
      }
      case deliverypolicy::Shape::ClassA:
        return this->fire_class_a_(f, what);
      }
      return false;
    }

    // C2 when the hub has a usable uplink stamp for this node, today's burst
    // otherwise. Not a switch: the fallback IS the old behaviour, so a node the hub
    // cannot place still gets its frame the way it always did.
    bool LORAListener::fire_class_a_(const MacFrame &f, const char *what)
    {
      const ClassAPlacement placement =
          this->send_into_class_a_window_(f.bytes.data(), f.bytes.size(), f.seal);
      if (placement == ClassAPlacement::Placed)
        return true;

      if (placement == ClassAPlacement::WindowsPast)
      {
        // The node is ASLEEP, and this end is the one that knows it.
        //
        // The burst below rests on "a TimeSync carries no ack, so a lost
        // single copy is silent" — an argument that assumes something is
        // LISTENING. Both Class A windows have closed: no number of copies
        // is heard, so 17 of them is 1408 ms of air, colliding with every
        // other node's window, to achieve exactly nothing.
        //
        // Dropping costs nothing the burst would have won. The node beacons
        // on its next wake and P2b answers every beacon with a TimeSync
        // 750 ms later — while RX1 is still 250 ms out, so THAT one places.
        ESP_LOGI(TAG, "[%s] %s dropped — both Class A windows past, "
                      "the node is asleep; its next beacon gets one",
                 this->get_name().c_str(), what);
        return false;
      }
      if (placement == ClassAPlacement::NotClassA ||
          placement == ClassAPlacement::NoUplinkStamp)
      {
        // PLACED, and as a BURST. Placement is the fix: through the bare
        // send() this left whenever the queue drained, so in Mode B it
        // arrived outside the node's window and the clock silently did not
        // advance.
        //
        // The burst is asked for EXPLICITLY because a TimeSync carries no
        // ack. Rule 4's exposure argument — "being wrong costs one frame" —
        // rests on the loss being visible; for an unacked frame a missed
        // single copy is silent, and the node runs on a stale clock until
        // the next push. Airtime is the cheaper side of that trade.
        //
        // Both reasons are named rather than negated: these two are the
        // cases where the hub does NOT know where the node listens, and the
        // burst spans every window it might open. WindowsPast is the case
        // where it knows precisely, and knows the answer is "nowhere".
        TxPolicy p;
        p.copies = this->parent_->defaultBurstCopies();
        this->send_aligned_(f.bytes.data(), f.bytes.size(), p, f.seal);
        return true;
      }
      return false;   // NotSendable: nothing to send
    }

    // ONE attempt of the slot's request (S7). A Resend::SameBytes retry re-sends
    // the stored first frame byte for byte under its ORIGINAL msgid -- that is the
    // point, a fresh msgid would be a new command to the node. Every other attempt
    // asks the application for the payload, frames it under a fresh msgid and
    // places it. A SameBytes request whose stored frame is gone (a session change
    // cleared it) is re-packed: a NEW command, so its id range moves with it
    // (security review finding 1: the old range would reject the node's ack).
    // Returns the msgid, 0 when nothing was framed.
    uint32_t LORAListener::dlv_attempt_(DeliverySlot &s, bool retry, uint32_t *tag)
    {
      const bool same_bytes = s.req.policy.resend == deliverypolicy::Resend::SameBytes;
      if (retry && same_bytes && !s.frame.bytes.empty())
      {
        *tag = s.frame_tag;
        if (s.req.place)
          s.req.place(s.frame, DeliveryAttempt{true, true, s.frame_tag});
        return s.frame.msgid;
      }
      if (retry && same_bytes)
        ESP_LOGW(TAG, "[%s] no stored frame to retransmit — re-packing", this->get_name().c_str());
      MacFrame f;
      bool framed = false;
      const DeliveryRequest::FrameFn frame = [this, &f, &framed](hubframes::Down *op) {
        framed = this->mac_frame_(op, &f);
        return framed;
      };
      if (s.req.encode)
        s.req.encode(retry, tag, frame);
      if (framed && s.req.place)
        s.req.place(f, DeliveryAttempt{retry, false, *tag});
      if (same_bytes)
      {
        // Only a frame that was built is ever stored; a failed pack leaves the
        // slot empty (deliver_ cleared it), so the retry re-packs or abandons.
        if (framed)
        {
          s.frame     = f;
          s.frame_tag = *tag;
        }
        if (retry)
          s.first_msgid = f.msgid;   // a re-pack is a new command: its range moves
        return f.msgid;              // a SameBytes id is taken even when packing failed
      }
      return framed ? f.msgid : 0;
    }

    // The attempt lines of a Resend::SameBytes request (the Control channel).
    void LORAListener::dlv_log_attempt_(const DeliverySlot &s, bool retry, uint32_t msgid)
    {
      if (s.req.policy.resend != deliverypolicy::Resend::SameBytes)
        return;
      if (!retry)
      {
        if (s.req.sent_label != nullptr)
          ESP_LOGI(TAG, "[%s] %s sent (msgid=%u) — awaiting ack", this->get_name().c_str(),
                   s.req.sent_label, (unsigned) msgid);
      }
      else if (s.short_wait_spent && s.retry_count == 0)
        ESP_LOGW(TAG, "[%s] %s burst fallback (msgid=%u)", this->get_name().c_str(), s.req.what,
                 (unsigned) msgid);
      else
        ESP_LOGW(TAG, "[%s] %s retransmit %u/%u (msgid=%u)", this->get_name().c_str(), s.req.what,
                 (unsigned) s.retry_count, (unsigned) s.req.policy.max_retries, (unsigned) msgid);
    }

    bool LORAListener::dlv_tx_(DeliveryChannel ch, bool retry)
    {
      DeliverySlot &s = this->slots_[ch];
      uint32_t tag = 0;
      const uint32_t msgid = this->dlv_attempt_(s, retry, &tag);
      this->dlv_log_attempt_(s, retry, msgid);
      if (msgid != 0)
      {
        s.acks.add(msgid, tag);
        s.last_msgid = msgid;
        if (!retry)
          s.first_msgid = msgid;
      }
      // A retry that could not be built still keeps the ladder running to its
      // end; a first attempt that could not be built starts nothing.
      if (s.req.policy.mode == deliverypolicy::Mode::Acked && (msgid != 0 || retry))
      {
        s.awaiting_ack = true;
        this->arm_dlv_timer_(ch);
      }
      return msgid != 0;
    }

    void LORAListener::arm_dlv_timer_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      // The FIRST wait of an optimistic single shot is the short, adaptive one
      // (SingleShotAckWait.h), measured from the placed mark; every other wait is
      // the policy's, plus the transmit backlog.
      const bool short_wait = deliverypolicy::isShortFirstWait(
          s.sent_optimistic, s.retry_count, s.short_wait_spent);
      s.first_tail_ms = 0;
      const uint32_t wait_ms = short_wait ? this->dlv_short_wait_ms_(ch)
                                          : this->ack_wait_ms_(s.req.policy.base_wait_ms);
      this->set_timeout(dlv_timer_(ch), wait_ms,
                        [this, ch, short_wait]() { this->on_dlv_timeout_(ch, short_wait); });
    }

    // The short first ack wait of an optimistic single shot, in ms from NOW.
    // Records the node's mark->ack tail in first_tail_ms.
    uint32_t LORAListener::dlv_short_wait_ms_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      // From NOW: whichever is later of the placed mark and the queue's drain
      // (they estimate the same instant; ack_wait_ms_ must NOT be added on top,
      // it would count the time to the mark twice), plus THIS node's measured
      // mark->ack tail.
      const int64_t  us_to_t0 = ((s.mark_us != 0) ? s.mark_us : this->last_placed_t0_us_) -
                                esp_timer_get_time();
      int64_t drain_ms = (this->parent_ != nullptr) ? this->parent_->txDrainUs() / 1000 : 0;
      if (drain_ms < 0)
        drain_ms = 0;
      if (drain_ms > (int64_t) kMaxAckBacklogMs)
        drain_ms = kMaxAckBacklogMs;
      const uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
      const uint32_t tail   = this->ack_latency_.tailMs(now_ms);
      s.first_tail_ms = tail;
      const uint32_t wait_ms = singleshotwait::firstAckWaitMs(us_to_t0, (uint32_t) drain_ms, tail);
      ESP_LOGI(TAG, "[%s] optimistic single shot: ack deadline %u ms from now "
                    "(mark in %d ms, queue drain %d ms, tail %u ms; node history %u "
                    "samples, max %u ms)",
               this->get_name().c_str(), (unsigned) wait_ms,
               (int) (us_to_t0 / 1000), (int) drain_ms, (unsigned) tail,
               (unsigned) this->ack_latency_.count(now_ms),
               (unsigned) this->ack_latency_.maxDelayMs(now_ms));
      return wait_ms;
    }

    void LORAListener::on_dlv_timeout_(DeliveryChannel ch, bool short_wait)
    {
      DeliverySlot &s = this->slots_[ch];
      const deliverypolicy::TimeoutDecision d = deliverypolicy::onAckTimeout(
          {s.awaiting_ack, this->relogin_pending_, short_wait, s.retry_count, s.req.policy.max_retries});
      s.retry_count = d.retries_spent;
      switch (d.action)
      {
      case deliverypolicy::TimeoutAction::Stale:
        return;   // acked already; one-shot, nothing to re-arm
      case deliverypolicy::TimeoutAction::DeferRelogin:
        // No session to answer on (a relogin can start WHILE a retry is in
        // flight, and clears the stored frame): a frame sent now would be a
        // plaintext one the node, still holding its old session, rejects. Wait for
        // confirm_session_(), charge nothing.
        ESP_LOGD(TAG, "[%s] %s retry deferred — session being rebuilt", this->get_name().c_str(),
                 s.req.what);
        this->arm_dlv_timer_(ch);
        return;
      case deliverypolicy::TimeoutAction::ShortWaitMissed:
        this->dlv_short_wait_missed_(ch);
        return;
      case deliverypolicy::TimeoutAction::GiveUp:
        this->dlv_give_up_(ch);
        return;
      case deliverypolicy::TimeoutAction::Retransmit:
        break;
      }
      this->dlv_resend_(ch);
    }

    void LORAListener::dlv_resend_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      if (s.req.resendable && !s.req.resendable())
      {
        this->dlv_abandon_(ch);
        return;
      }
      // Rule 4: a single shot that went unacked puts this node back on bursts
      // immediately, and stays that way until a fresh in-slot uplink. One frame is
      // the whole exposure -- the retry below is already a burst.
      if (deliverypolicy::marksSingleShotUnacked(s.sent_single_shot, this->belief_.single_shot_unacked))
      {
        this->belief_.single_shot_unacked = true;
        ESP_LOGW(TAG, "[%s] single shot unacked — back to bursts until "
                      "the node is confirmed again", this->get_name().c_str());
      }
      if (s.req.resend_verb != nullptr)
        ESP_LOGW(TAG, "[%s] %s not acknowledged — %s (%u/%u)", this->get_name().c_str(),
                 s.req.what, s.req.resend_verb, (unsigned) s.retry_count,
                 (unsigned) s.req.policy.max_retries);
      this->dlv_tx_(ch, true);
    }

    void LORAListener::dlv_done_(DeliveryChannel ch)
    {
      const std::function<void()> done = this->slots_[ch].req.on_done;
      if (done)
        done();
    }

    void LORAListener::dlv_abandon_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.awaiting_ack = false;
      ESP_LOGE(TAG, "[%s] %s may already have been heard and can no longer be re-sent as the "
                    "same command (its session changed) — failed, NOT re-sent",
               this->get_name().c_str(), s.req.what);
      if (s.req.on_abandon)
        s.req.on_abandon();
      else if (s.req.on_fail)
        s.req.on_fail();
      this->dlv_done_(ch);
    }

    // The optimistic guess missed. Burst NOW (Rule 4 marks the node unacked, so
    // the resend places a full burst), but do NOT count it: retry_count is the
    // budget that ends in "command failed, tear the session down", and a guess the
    // operator opted into is not evidence the session is dead. The normal waits and
    // the full budget follow unchanged.
    void LORAListener::dlv_short_wait_missed_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.short_wait_spent = true;
      this->belief_.single_shot_unacked = true;
      // The tail just proven too short is a lower bound on this node's delay:
      // remember it, so the next deadline is longer instead of the same mistake
      // (and a node that never answers climbs to the normal wait, no further).
      this->ack_latency_.note((uint32_t) (esp_timer_get_time() / 1000), s.first_tail_ms);
      ESP_LOGW(TAG, "[%s] optimistic single shot unacked — bursting now "
                    "(not counted against the retry budget)",
               this->get_name().c_str());
      this->dlv_tx_(ch, true);
    }

    void LORAListener::dlv_give_up_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.awaiting_ack = false;
      if (s.req.on_fail)
        s.req.on_fail();
      if (s.req.policy.give_up == deliverypolicy::GiveUpAction::FailRelogin)
      {
        // Session recovery. A command that goes fully unacked means the session is
        // effectively dead -- the hub never confirmed it, OR it had, but the node
        // has since re-keyed/hung leaving a STALE-confirmed flag (observed
        // 2026-07-11: node 1 failed 4x without recovering because session_confirmed_
        // was still true). Clear the confirmation and force an immediate fresh login
        // so the next command rebuilds the session in seconds.
        this->session_confirmed_ = false;
        ESP_LOGW(TAG, "[%s] Command failed after retries — clearing session, forcing re-login",
                 this->get_name().c_str());
        this->do_login_and_arm_retry_();
      }
      this->dlv_done_(ch);
    }

    // The node acknowledged `ack_msg_id`. A request that owns it is told which
    // attempt was answered (its tag) and says whether that completes it.
    bool LORAListener::dlv_ack_(uint32_t ack_msg_id)
    {
      // Highest channel first: a GridSync takes an id before a schedule push would.
      for (uint8_t c = kDeliveryChannels; c-- > 0;)
      {
        DeliverySlot &s = this->slots_[c];
        const int i = s.acks.find(ack_msg_id);
        if (i < 0)
          continue;
        if (!s.req.on_delivered || s.req.on_delivered(DeliveryConfirm{s.acks.tag[i], ack_msg_id}))
        {
          this->dlv_cancel_((DeliveryChannel) c);
          this->dlv_done_((DeliveryChannel) c);
        }
        return true;
      }
      return false;
    }

    void LORAListener::dlv_cancel_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      this->cancel_timeout(dlv_timer_(ch));
      s.awaiting_ack = false;
      s.retry_count  = 0;
      s.acks.clear();
    }

    // A new login restarts the msgid space, so ids on record would alias new ones
    // -- on every channel, the Control channel included (review HB15: a command
    // waiting behind the relogin kept old-session ids a new-session FCNT could
    // match). The requests themselves go on (the next attempt is sent under a
    // new id).
    void LORAListener::dlv_forget_ids_()
    {
      for (uint8_t c = 0; c < kDeliveryChannels; ++c)
        this->slots_[c].acks.clear();
    }

    bool LORAListener::delivery_busy_() const
    {
      return this->ctl().awaiting_ack || this->ctl().deferred_until_login ||
             this->cfg().awaiting_ack || this->mac().awaiting_ack;
    }

    // Mark -> ack, for every command whose FIRST frame was a single placed
    // copy (one INFO per user command: this is not a periodic log). It feeds
    // the node's latency history only when clean (no retry, no fallback burst).
    void LORAListener::record_ack_latency_()
    {
      const int64_t now_us   = esp_timer_get_time();
      const int64_t delay_ms = (now_us - this->ctl().mark_us) / 1000;
      const bool clean = singleshotwait::isCleanSample(
          true, this->ctl().retry_count, this->ctl().short_wait_spent, true);
      bool kept = false;
      if (clean && delay_ms >= 0)
        kept = this->ack_latency_.note((uint32_t) (now_us / 1000), (uint32_t) delay_ms);
      ESP_LOGI(TAG, "[%s] ack %lld ms after the mark (%s%s; first-ack tail %u ms; "
                    "history now %u samples, max %u ms)",
               this->get_name().c_str(), (long long) delay_ms,
               clean ? "clean single shot" : "after a burst fallback or retry",
               kept ? ", recorded" : ", not recorded", (unsigned) this->ctl().first_tail_ms,
               (unsigned) this->ack_latency_.count((uint32_t) (now_us / 1000)),
               (unsigned) this->ack_latency_.maxDelayMs((uint32_t) (now_us / 1000)));
    }

    void LORAListener::handle_command_ack_(uint32_t ack_msg_id, AckStatus status)
    {
      this->acked_since_confirm_ = true;
      if (status == ACK_STATUS__ACK_OK)
        this->dlv_ack_(ack_msg_id);
      else
        this->dlv_refused_(ack_msg_id, status);
    }

    // The node HEARD an Acked frame and answered that it will not execute it:
    // ACK_ERROR for a body it cannot dispatch (a case newer than its firmware,
    // review P3), ACK_BUSY reserved. Retrying the same bytes cannot change that,
    // and the session is demonstrably alive, so the request fails at once:
    // no retries, no relogin (unlike a ladder that ran out without any answer).
    bool LORAListener::dlv_refused_(uint32_t ack_msg_id, AckStatus status)
    {
      for (uint8_t c = kDeliveryChannels; c-- > 0;)
      {
        DeliverySlot &s = this->slots_[c];
        if (s.acks.find(ack_msg_id) < 0)
          continue;
        ESP_LOGE(TAG, "[%s] %s refused by the node (status %d, FCNT %u) — failed, not retried",
                 this->get_name().c_str(), s.req.what, (int) status, (unsigned) ack_msg_id);
        this->dlv_cancel_((DeliveryChannel) c);
        if (s.req.on_abandon)
          s.req.on_abandon();
        else if (s.req.on_fail)
          s.req.on_fail();
        this->dlv_done_((DeliveryChannel) c);
        return true;
      }
      return false;
    }

    uint32_t LORAListener::ack_wait_ms_(uint32_t base_ms) const
    {
      int64_t drain_ms = (this->parent_ != nullptr) ? this->parent_->txDrainUs() / 1000 : 0;
      if (drain_ms < 0)
        drain_ms = 0;
      if (drain_ms > (int64_t) kMaxAckBacklogMs)
        drain_ms = kMaxAckBacklogMs;
      return base_ms + (uint32_t) drain_ms;
    }

  }  // namespace lora_tracker
}  // namespace esphome
